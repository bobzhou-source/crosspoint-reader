"""Read one X post/thread from the Chrome session that is already signed in.

Only what Chrome's own page loads is used: this module creates one tab through the
DevTools endpoint, navigates it to the requested post, reads the post data X returns
for that page, and closes that tab again. Cookies, Authorization headers and other
tabs are never read, stored or forwarded; the signed-in session stays in the browser.
The result is the same ``{data, includes}`` API v2 shape the X adapter already parses.

The DevTools transport below (``DEFAULT_CDP_ENDPOINT``, ``cdp_endpoint``, ``_devtools``,
``_new_target``, ``_close_target``, ``_Session``, ``_Timeout``) is a stable internal contract:
other browser sources in this package reuse it instead of shipping their own WebSocket client.
Keep those names and signatures, and keep error text here site-neutral.
"""

from __future__ import annotations

import base64
import json
import re
import time
from datetime import datetime
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, quote, urlsplit
from urllib.request import Request, urlopen

from .common import MAX_HTML_BYTES, ReaderError, http_url
from .x_adapter import post_id

DEFAULT_CDP_ENDPOINT = "http://127.0.0.1:9333"
LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "::1"}
TWEET_DETAIL_PATH = re.compile(r"/i/api/graphql/[^/]+/TweetDetail(?:$|\?)")
HOME_FOLLOWING_PATH = re.compile(r"/i/api/graphql/[^/]+/HomeLatestTimeline(?:$|\?)")
LOGIN_PATH = re.compile(r"^/i/(?:flow/(?:login|signup)|jf/onboarding)")
UNAVAILABLE = re.compile(r"this post is unavailable|no longer available|limits who can view|"
                         r"not authorized to view|something went wrong|account doesn't exist", re.I)

# Read-only snapshot of the rendered post page. The first status link of an article is
# that article's own post (photo links repeat it); a quoted post's link never comes first.
DOM_SNAPSHOT_JS = r"""(() => {
  const articles = [...document.querySelectorAll('article[data-testid="tweet"]')];
  const idOf = (href) => { const match = /\/status\/(\d+)/.exec(href || ""); return match ? match[1] : ""; };
  return {
    href: location.href,
    signedIn: !!document.querySelector('[data-testid="SideNav_AccountSwitcher_Button"]'),
    loginPrompt: !!document.querySelector('[data-testid="loginButton"]')
      || [...document.querySelectorAll("h1,h2,span")].some((node) => /^Sign in to X$/i.test((node.innerText || "").trim())),
    bodyText: (document.body ? document.body.innerText : "").replace(/\s+/g, " ").slice(0, 800),
    articles: articles.slice(0, 12).map((article) => {
      const links = [...article.querySelectorAll('a[href*="/status/"]')].map((a) => a.getAttribute("href") || "");
      const nameNode = article.querySelector('[data-testid="User-Name"]');
      const time = article.querySelector("time");
      return {
        id: idOf(links[0]),
        name: nameNode ? (nameNode.innerText || "").replace(/\s+/g, " ").trim() : "",
        datetime: time ? time.getAttribute("datetime") || "" : "",
        text: [...article.querySelectorAll('[data-testid="tweetText"]')].map((n) => n.innerText || "").join("\n").trim(),
        photos: [...article.querySelectorAll('[data-testid="tweetPhoto"] img')]
          .map((i) => i.getAttribute("src") || "").filter(Boolean).slice(0, 4)
      };
    })
  };
})()"""


class _Timeout(Exception):
    """The capture deadline passed while waiting for Chrome."""


def cdp_endpoint(value: str = DEFAULT_CDP_ENDPOINT) -> str:
    parts = urlsplit(str(value).strip())
    try:
        port = parts.port
    except ValueError as exc:
        raise ReaderError(f"Invalid DevTools port in {value!r}; expected http://127.0.0.1:9333") from exc
    if parts.scheme != "http" or parts.username is not None or parts.password is not None:
        raise ReaderError("Browser capture needs a plain loopback DevTools endpoint such as http://127.0.0.1:9333")
    host = (parts.hostname or "").lower()
    if host not in LOOPBACK_HOSTS:
        raise ReaderError("Browser capture only connects to a loopback Chrome DevTools endpoint "
                          "(127.0.0.1); other hosts are refused")
    if parts.path not in {"", "/"} or parts.query or parts.fragment:
        raise ReaderError("A DevTools endpoint is only scheme://host:port; remove the path, query or fragment")
    if not port:
        raise ReaderError("Include the DevTools port, e.g. http://127.0.0.1:9333")
    return f"http://[::1]:{port}" if ":" in host else f"http://{host}:{port}"


def _http(base: str, path: str, *, method: str = "GET", timeout: float) -> bytes:
    request = Request(base + path, method=method)
    try:
        with urlopen(request, timeout=timeout) as response:
            data = response.read(MAX_HTML_BYTES + 1)
    except HTTPError as exc:
        detail = exc.read(256).decode("utf-8", "replace").strip()
        raise ReaderError(f"Chrome DevTools {method} {path} failed with HTTP {exc.code}: {detail}") from exc
    except (URLError, OSError) as exc:
        port = urlsplit(base).port
        raise ReaderError(f"Cannot reach Chrome DevTools at {base}: {exc}. Start Chrome with "
                          f"--remote-debugging-port={port} in a profile that is signed in to the site you need, "
                          "and retry") from exc
    if len(data) > MAX_HTML_BYTES:
        raise ReaderError("Chrome DevTools response exceeds 8 MiB")
    return data


def _devtools(base: str, timeout: float) -> dict:
    try:
        info = json.loads(_http(base, "/json/version", timeout=timeout))
    except (ValueError, UnicodeDecodeError) as exc:
        raise ReaderError(f"{base} is not a Chrome DevTools endpoint") from exc
    if not isinstance(info, dict) or not str(info.get("Browser", "")).startswith("Chrome"):
        raise ReaderError(f"{base} is not a Chrome DevTools endpoint")
    return info


def _new_target(base: str, timeout: float) -> dict:
    try:
        target = json.loads(_http(base, "/json/new?about:blank", method="PUT", timeout=timeout))
    except (ValueError, UnicodeDecodeError) as exc:
        raise ReaderError("Chrome returned an unreadable new-tab response") from exc
    if not isinstance(target, dict) or not target.get("id") or not target.get("webSocketDebuggerUrl"):
        raise ReaderError("Chrome did not hand out a usable tab for browser capture")
    return target


def _close_target(base: str, identifier: str, timeout: float) -> None:
    _http(base, "/json/close/" + quote(str(identifier), safe=""), timeout=timeout)


class _Session:
    """Minimal synchronous DevTools client: one call at a time, events buffered."""

    def __init__(self, websocket_url: str, timeout: float):
        try:
            from websockets.sync.client import connect
        except ImportError as exc:
            raise ReaderError("Browser capture needs the optional websockets package: "
                              "pip install 'reader-workbench-companion[browser]'") from exc
        try:
            self._socket = connect(websocket_url, open_timeout=max(1.0, timeout),
                                   max_size=MAX_HTML_BYTES, close_timeout=5)
        except Exception as exc:
            raise ReaderError(f"Cannot attach to the new Chrome tab: {exc}") from exc
        self._next_id = 0
        self.events: list[dict] = []

    def close(self) -> None:
        try:
            self._socket.close()
        except Exception:
            pass

    def _read(self, deadline: float) -> dict:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise _Timeout
        try:
            message = self._socket.recv(timeout=remaining)
        except TimeoutError as exc:
            raise _Timeout from exc
        except Exception as exc:
            raise ReaderError(f"Chrome closed the capture tab: {exc}") from exc
        try:
            payload = json.loads(message)
        except (ValueError, UnicodeDecodeError) as exc:
            raise ReaderError("Chrome sent an unreadable DevTools message") from exc
        if not isinstance(payload, dict):
            raise ReaderError("Chrome sent an unreadable DevTools message")
        return payload

    def call(self, method: str, params: dict | None, deadline: float) -> dict:
        self._next_id += 1
        identifier = self._next_id
        self._socket.send(json.dumps({"id": identifier, "method": method, "params": params or {}}))
        while True:
            message = self._read(deadline)
            if message.get("id") == identifier:
                if message.get("error"):
                    error = message["error"]
                    raise ReaderError(f"Chrome {method} failed: {error.get('message', error)}")
                return message.get("result") or {}
            if message.get("method"):
                self.events.append(message)

    def next_event(self, deadline: float) -> dict:
        if self.events:
            return self.events.pop(0)
        while True:
            message = self._read(deadline)
            if message.get("method"):
                return message


def _matches_focal(url: str, requested: str) -> bool:
    values = parse_qs(urlsplit(url).query).get("variables")
    if not values:
        return True
    try:
        variables = json.loads(values[0])
    except ValueError:
        return True
    focal = str(variables.get("focalTweetId", "")) if isinstance(variables, dict) else ""
    return not focal or focal == requested


def _walk_tweets(value, found: dict) -> None:
    if isinstance(value, dict):
        if (re.fullmatch(r"\d+", str(value.get("rest_id", "")))
                and isinstance(value.get("legacy"), dict) and "conversation_id_str" in value["legacy"]):
            found.setdefault(str(value["rest_id"]), value)
        for child in value.values():
            _walk_tweets(child, found)
    elif isinstance(value, list):
        for child in value:
            _walk_tweets(child, found)


def _raw_tweets(payloads: list) -> dict:
    found: dict = {}
    for payload in payloads:
        _walk_tweets(payload, found)
    return found


def _iso(value: str) -> str:
    if not value:
        return ""
    try:
        return datetime.strptime(value, "%a %b %d %H:%M:%S %z %Y").isoformat().replace("+00:00", "Z")
    except ValueError:
        return value


def _unwrap(result):
    for _ in range(4):
        if not isinstance(result, dict):
            return None
        if isinstance(result.get("legacy"), dict):
            return result
        if isinstance(result.get("tweet"), dict):
            result = result["tweet"]
            continue
        return None
    return None


def _user(result):
    if not isinstance(result, dict):
        return None
    if isinstance(result.get("core"), dict) or isinstance(result.get("legacy"), dict):
        return result
    if isinstance(result.get("user"), dict):
        return result["user"]
    return None


def _profile(result) -> dict | None:
    user = _user(result)
    if user is None:
        return None
    core = user.get("core") if isinstance(user.get("core"), dict) else {}
    legacy = user.get("legacy") if isinstance(user.get("legacy"), dict) else {}
    name = str(core.get("name") or legacy.get("name") or "")
    screen = str(core.get("screen_name") or legacy.get("screen_name") or "")
    if not name and not screen:
        return None
    return {"name": name, "username": screen}


def _entity_set(note: dict, legacy: dict) -> dict:
    entities = note.get("entity_set") if isinstance(note.get("entity_set"), dict) else {}
    base = legacy.get("entities") if isinstance(legacy.get("entities"), dict) else {}
    urls = entities.get("urls") or base.get("urls") or []
    return {**base, "urls": urls}


def _api_tweet(raw: dict, users: dict, media: dict) -> dict:
    legacy = raw["legacy"]
    ident = str(raw["rest_id"])
    user = _user((raw.get("core") or {}).get("user_results", {}).get("result"))
    author_id = str(legacy.get("user_id_str") or (user or {}).get("rest_id") or "")
    if user is not None and author_id:
        profile = user.get("core") if isinstance(user.get("core"), dict) else user.get("legacy", {})
        users.setdefault(author_id, {"id": author_id,
                                     "name": str(profile.get("name") or ""),
                                     "username": str(profile.get("screen_name") or "")})
    quoted = _unwrap((raw.get("quoted_status_result") or {}).get("result"))
    referenced = []
    if legacy.get("in_reply_to_status_id_str"):
        referenced.append({"type": "replied_to", "id": str(legacy["in_reply_to_status_id_str"])})
    quoted_id = str((quoted or {}).get("rest_id") or legacy.get("quoted_status_id_str") or "")
    if re.fullmatch(r"\d+", quoted_id):
        referenced.append({"type": "quoted", "id": quoted_id})
    note_result = ((raw.get("note_tweet") or {}).get("note_tweet_results") or {}).get("result")
    note = {}
    if isinstance(note_result, dict) and str(note_result.get("text") or ""):
        note = {"text": str(note_result["text"]), "entities": _entity_set(note_result, legacy)}
    keys = []
    attachments = {}
    for entry in ((legacy.get("extended_entities") or {}).get("media")
                  or (legacy.get("entities") or {}).get("media") or []):
        if not isinstance(entry, dict):
            continue
        key = str(entry.get("media_key") or entry.get("id_str") or "")
        if not key:
            continue
        keys.append(key)
        photo = entry.get("type") == "photo"
        media[key] = {"media_key": key, "type": str(entry.get("type") or "photo"),
                      "url": str(entry.get("media_url_https") or "") if photo else "",
                      "preview_image_url": str(entry.get("media_url_https") or ""),
                      "alt_text": str(entry.get("ext_alt_text") or "")}
    if keys:
        attachments["media_keys"] = keys
    return {"id": ident, "text": str(legacy.get("full_text") or ""), "author_id": author_id,
            "created_at": _iso(str(legacy.get("created_at") or "")),
            "conversation_id": str(legacy.get("conversation_id_str") or ident),
            "referenced_tweets": referenced, "entities": legacy.get("entities") or {},
            "attachments": attachments, "note_tweet": note}


def _has_show_more(payloads: list) -> bool:
    stack = list(payloads)
    while stack:
        value = stack.pop()
        if isinstance(value, dict):
            if str(value.get("cursorType", "")).startswith("ShowMore"):
                return True
            stack.extend(value.values())
        elif isinstance(value, list):
            stack.extend(value)
    return False


def _empty_placeholder(payloads: list, requested: str) -> bool:
    """X renders a titled but empty entry for posts it will not show."""
    stack = list(payloads)
    while stack:
        value = stack.pop()
        if isinstance(value, dict):
            content = value.get("content") if isinstance(value.get("content"), dict) else {}
            item = content.get("itemContent") if isinstance(content.get("itemContent"), dict) else {}
            if value.get("entryId") == f"tweet-{requested}" and \
                    item.get("tweet_results") in ({}, None):
                return True
            stack.extend(value.values())
        elif isinstance(value, list):
            stack.extend(value)
    return False


def api_payload(payloads: list, requested: str, *, include_thread: bool = True) -> dict | None:
    """Fold the page data X returned into the API v2 shape the X adapter reads.

    Only the requested post, other posts by the same author in the same conversation,
    and directly quoted posts survive; replies by other people are dropped. With
    ``include_thread=False`` (timeline items) only the post and its quotes are kept.
    """
    raws = _raw_tweets(payloads)
    if requested not in raws:
        return None
    users: dict = {}
    media: dict = {}
    tweets = {ident: _api_tweet(raw, users, media) for ident, raw in raws.items()}
    target = tweets[requested]
    author_id = str(target.get("author_id") or "")
    if not author_id:
        raise ReaderError("X returned the requested post without an author; cannot build the thread")
    conversation = str(target.get("conversation_id") or requested)
    keep_ids = {requested}
    if include_thread:
        for ident, tweet in tweets.items():
            if ident != requested and str(tweet.get("author_id")) == author_id \
                    and str(tweet.get("conversation_id")) == conversation:
                keep_ids.add(ident)
    quoted_ids = {ref["id"] for ident in keep_ids for ref in tweets[ident]["referenced_tweets"]
                  if ref["type"] == "quoted"}
    keep_ids |= {ident for ident in quoted_ids if ident in tweets}
    kept_media = {key for ident in keep_ids for key in tweets[ident]["attachments"].get("media_keys", [])}
    payload = {
        "data": [target],
        "includes": {
            "tweets": [tweets[ident] for ident in sorted(keep_ids - {requested}, key=int)],
            "users": [users[ident] for ident in sorted(users, key=int)
                      if ident in {tweets[other]["author_id"] for other in keep_ids}],
            "media": [media[key] for key in sorted(kept_media) if key in media],
        },
    }
    if _has_show_more(payloads):
        payload["_warnings"] = ["X rendered only the first batch of replies for this thread; later "
                                "posts were not loaded by the browser, so this EPUB may stop early."]
    return payload


def _payload_from_dom(dom: dict, requested: str) -> dict | None:
    for article in dom.get("articles") or []:
        if not isinstance(article, dict) or str(article.get("id") or "") != requested:
            continue
        text = str(article.get("text") or "").strip()
        if not text:
            return None
        name = str(article.get("name") or "")
        match = re.search(r"@([A-Za-z0-9_]{1,15})", name)
        username = match.group(1) if match else "i/web"
        display = name.split("@")[0].strip() or username
        tweet = {"id": requested, "text": text, "author_id": username,
                 "created_at": str(article.get("datetime") or ""), "conversation_id": requested,
                 "referenced_tweets": [], "entities": {}, "attachments": {}, "note_tweet": {}}
        media = []
        for index, source in enumerate([item for item in (article.get("photos") or [])
                                        if isinstance(item, str)]):
            try:
                address = http_url(source)
            except ReaderError:
                continue
            key = f"dom:{requested}:{index}"
            media.append({"media_key": key, "type": "photo", "url": address,
                          "preview_image_url": address, "alt_text": ""})
            tweet["attachments"].setdefault("media_keys", []).append(key)
        return {"data": [tweet],
                "includes": {"tweets": [], "media": media,
                             "users": [{"id": username, "name": display, "username": username}]},
                "_warnings": ["X did not return post data for this page; this EPUB uses only the text "
                              "rendered on the post page and may omit the rest of the thread."]}
    return None


def _read_payload(session: _Session, request_id: str, deadline: float) -> dict | None:
    try:
        result = session.call("Network.getResponseBody", {"requestId": request_id}, deadline)
    except ReaderError:
        return None
    body = result.get("body") or ""
    if result.get("base64Encoded"):
        try:
            body = base64.b64decode(body).decode("utf-8", "replace")
        except (ValueError, TypeError):
            return None
    if not body or len(body) > MAX_HTML_BYTES:
        return None
    try:
        payload = json.loads(body)
    except ValueError:
        return None
    return payload if isinstance(payload, dict) else None


def _dom_snapshot(session: _Session, deadline: float) -> dict:
    try:
        result = session.call("Runtime.evaluate",
                              {"expression": DOM_SNAPSHOT_JS, "returnByValue": True}, deadline)
    except (_Timeout, ReaderError):
        return {}
    value = (result.get("result") or {}).get("value")
    return value if isinstance(value, dict) else {}


def _capture(session: _Session, url: str, requested: str, deadline: float) -> tuple[list, dict, str]:
    session.call("Page.enable", {}, deadline)
    session.call("Network.enable", {}, deadline)
    navigate = session.call("Page.navigate", {"url": url}, deadline)
    if navigate.get("errorText"):
        raise ReaderError(f"Chrome could not open {url}: {navigate['errorText']}")
    responses: list = []
    page_url = url
    pending: dict = {}
    try:
        while time.monotonic() < deadline:
            event = session.next_event(deadline)
            method = event["method"]
            params = event.get("params") or {}
            if method == "Network.responseReceived":
                response = params.get("response") or {}
                address = str(response.get("url") or "")
                if TWEET_DETAIL_PATH.search(address) and _matches_focal(address, requested):
                    pending[str(params.get("requestId") or "")] = address
            elif method == "Network.loadingFinished":
                request_id = str(params.get("requestId") or "")
                if request_id in pending:
                    pending.pop(request_id, None)
                    payload = _read_payload(session, request_id, deadline)
                    if payload is not None:
                        responses.append(payload)
                        # The focal TweetDetail response is the page's whole first batch:
                        # either it carries the requested post or X has none to show.
                        break
            elif method == "Page.frameNavigated":
                frame = params.get("frame") or {}
                if not frame.get("parentId") and frame.get("url"):
                    page_url = str(frame["url"])
    except _Timeout:
        pass
    dom = _dom_snapshot(session, deadline) if requested not in _raw_tweets(responses) else {}
    return responses, dom, page_url


def _failure(url: str, requested: str, page_url: str, responses: list, dom: dict,
             timeout: float) -> ReaderError:
    path = urlsplit(page_url).path
    if dom.get("loginPrompt") or LOGIN_PATH.match(path):
        return ReaderError("X asked Chrome to sign in before showing that post. Sign in to X in the agent "
                           "Chrome window (port 9333), then retry with --browser; no API key or export is needed.")
    if dom and not dom.get("signedIn"):
        return ReaderError("That Chrome window has no signed-in X session, so X refused the post. Sign in to X "
                           "in the agent Chrome window (port 9333) and retry with --browser.")
    if UNAVAILABLE.search(str(dom.get("bodyText") or "")):
        return ReaderError(f"X reports that post {requested} is unavailable: it may be deleted, protected, "
                           "age-restricted, or not visible to this account.")
    if _empty_placeholder(responses, requested):
        return ReaderError(f"X shows no content for post {requested}: it was deleted, never existed, is from a "
                           "protected account, or this account may not view it. Copy the link from the post itself "
                           "and retry.")
    if not responses:
        return ReaderError(f"Chrome opened {url} but X returned no post data within {timeout:g}s. Check the "
                           "connection, then retry; if the tab is stuck, open the post in Chrome once and retry.")
    return ReaderError(f"X returned post data that does not contain the requested post {requested}; it may have "
                       "been deleted or restricted since the link was copied.")


def capture_x_url(url: str, *, endpoint: str = DEFAULT_CDP_ENDPOINT, timeout: float = 30.0) -> dict:
    """Return API v2-shaped post data for one X post/thread from the signed-in browser."""
    requested = post_id(url)
    origin = cdp_endpoint(endpoint)
    timeout = max(1.0, min(float(timeout), 300.0))
    deadline = time.monotonic() + timeout
    try:
        _devtools(origin, min(timeout, 5.0))
    except ReaderError as exc:
        raise ReaderError(f"{exc}. For X, use Chrome with the agent profile "
                          "(--user-data-dir=$HOME/.job-app-chrome) and stay signed in to X; "
                          "no API key or export is needed.") from exc
    target = _new_target(origin, min(timeout, 10.0))
    session = None
    try:
        session = _Session(str(target["webSocketDebuggerUrl"]), timeout)
        try:
            responses, dom, page_url = _capture(session, url, requested, deadline)
        except _Timeout:
            responses, dom, page_url = [], {}, url
    finally:
        if session is not None:
            session.close()
        try:
            _close_target(origin, str(target["id"]), min(timeout, 5.0))
        except ReaderError:
            pass
    payload = api_payload(responses, requested)
    if payload is None:
        payload = _payload_from_dom(dom, requested)
    if payload is None:
        raise _failure(url, requested, page_url, responses, dom, timeout)
    return payload


FOLLOWING_TAB_JS = r"""(() => {
  const app = !!document.querySelector('[data-testid="SideNav_AccountSwitcher_Button"]');
  const tab = [...document.querySelectorAll('[role="tab"]')].find((node) => ((node.innerText || "").trim()) === "Following");
  const state = {
    signedIn: app,
    loginPrompt: !!document.querySelector('[data-testid="loginButton"]')
      || [...document.querySelectorAll("h1,h2,span")].some((node) => /^Sign in to X$/i.test((node.innerText || "").trim())),
    tab: !!tab,
    selected: false,
    href: location.href
  };
  if (tab) {
    const box = tab.getBoundingClientRect();
    state.selected = tab.getAttribute("aria-selected") === "true";
    state.x = box.x + box.width / 2;
    state.y = box.y + box.height / 2;
  }
  return state;
})()"""


def _evaluate(session: _Session, expression: str, deadline: float) -> dict:
    result = session.call("Runtime.evaluate", {"expression": expression, "returnByValue": True}, deadline)
    value = (result.get("result") or {}).get("value")
    return value if isinstance(value, dict) else {}


def _click(session: _Session, x: float, y: float, deadline: float) -> None:
    for kind in ("mousePressed", "mouseReleased"):
        session.call("Input.dispatchMouseEvent",
                     {"type": kind, "x": x, "y": y, "button": "left", "clickCount": 1}, deadline)


def capture_following_feed(*, endpoint: str = DEFAULT_CDP_ENDPOINT,
                           timeout: float = 45.0) -> list[dict]:
    """Return the HomeLatestTimeline responses X produced for the real Following tab."""
    origin = cdp_endpoint(endpoint)
    timeout = max(1.0, min(float(timeout), 300.0))
    deadline = time.monotonic() + timeout
    try:
        _devtools(origin, min(timeout, 5.0))
    except ReaderError as exc:
        raise ReaderError(f"{exc}. For X, use Chrome with the agent profile "
                          "(--user-data-dir=$HOME/.job-app-chrome) and stay signed in to X.") from exc
    target = _new_target(origin, min(timeout, 10.0))
    session = None
    payloads: list = []
    page_url = "https://x.com/home"
    try:
        session = _Session(str(target["webSocketDebuggerUrl"]), timeout)
        session.call("Page.enable", {}, deadline)
        session.call("Network.enable", {}, deadline)
        navigate = session.call("Page.navigate", {"url": "https://x.com/home"}, deadline)
        if navigate.get("errorText"):
            raise ReaderError(f"Chrome could not open X home: {navigate['errorText']}")
        pending: dict = {}
        clicked = False
        app_seen = False
        while not payloads and time.monotonic() < deadline:
            try:
                event = session.next_event(min(deadline, time.monotonic() + 0.3))
            except _Timeout:
                event = None
            if event is not None:
                method = event["method"]
                params = event.get("params") or {}
                if method == "Network.responseReceived":
                    address = str((params.get("response") or {}).get("url") or "")
                    if HOME_FOLLOWING_PATH.search(address):
                        pending[str(params.get("requestId") or "")] = address
                elif method == "Network.loadingFinished":
                    request_id = str(params.get("requestId") or "")
                    if request_id in pending:
                        pending.pop(request_id, None)
                        payload = _read_payload(session, request_id, deadline)
                        if payload is not None:
                            payloads.append(payload)
                            break
                elif method == "Page.frameNavigated":
                    frame = params.get("frame") or {}
                    if not frame.get("parentId") and frame.get("url"):
                        page_url = str(frame["url"])
                continue
            try:
                state = _evaluate(session, FOLLOWING_TAB_JS, deadline)
                if state.get("loginPrompt"):
                    raise ReaderError("X asked Chrome to sign in before showing the Following timeline. Sign in to X "
                                      "in the agent Chrome window (port 9333), then rerun this command.")
                if state.get("signedIn"):
                    app_seen = True
                    if state.get("tab") and not state.get("selected") and not clicked:
                        _click(session, float(state.get("x") or 0), float(state.get("y") or 0), deadline)
                        clicked = True
            except _Timeout:
                break
        if payloads:
            return payloads
    finally:
        if session is not None:
            session.close()
        try:
            _close_target(origin, str(target["id"]), min(timeout, 5.0))
        except ReaderError:
            pass
    if LOGIN_PATH.match(urlsplit(page_url).path):
        raise ReaderError("X redirected the Following timeline to a sign-in page. Sign in to X in the agent "
                          "Chrome window (port 9333), then rerun this command.")
    if app_seen:
        raise ReaderError("X rendered the home timeline but no Following timeline data arrived; open the "
                          "Following tab once in that Chrome window, then retry.")
    raise ReaderError("Chrome opened X home but the signed-in timeline never rendered. Check the connection and "
                      "that the Chrome window is signed in to X, then retry.")


def _instructions(payload, found: list | None = None) -> list:
    found = [] if found is None else found
    if isinstance(payload, dict):
        instructions = payload.get("instructions")
        if isinstance(instructions, list):
            found.extend(item for item in instructions if isinstance(item, dict))
        for child in payload.values():
            _instructions(child, found)
    elif isinstance(payload, list):
        for child in payload:
            _instructions(child, found)
    return found


def _item_contents(content: dict):
    direct = content.get("itemContent")
    if isinstance(direct, dict):
        yield direct
    for wrapper in content.get("items") or []:
        item = (wrapper or {}).get("item") if isinstance(wrapper, dict) else None
        if isinstance(item, dict) and isinstance(item.get("itemContent"), dict):
            yield item["itemContent"]


def _promoted(entry: dict) -> bool:
    content = entry.get("content") if isinstance(entry.get("content"), dict) else {}
    if str(entry.get("entryId") or "").startswith("promoted"):
        return True
    if content.get("promotedMetadata") or content.get("promoted_metadata"):
        return True
    event = content.get("clientEventInfo")
    return bool(event is not None and re.search(r"promot|advert", json.dumps(event), re.I))


def _timestamp(value) -> datetime | None:
    if not isinstance(value, str) or not value:
        return None
    try:
        return datetime.strptime(value, "%a %b %d %H:%M:%S %z %Y")
    except ValueError:
        return None


def feed_entries(payloads: list) -> list[dict]:
    """Flatten Following-timeline responses into posts, in the order X rendered them.

    Ads and promoted injections stay in the result marked ``promoted`` so callers can
    count them. A repost resolves to the original post (same canonical id) with the
    reposting account in ``reposted_by``. Conversation modules yield each post once.
    """
    entries: list[dict] = []
    for payload in payloads:
        for instruction in _instructions(payload):
            for entry in instruction.get("entries") or []:
                if not isinstance(entry, dict):
                    continue
                content = entry.get("content") if isinstance(entry.get("content"), dict) else {}
                if content.get("cursorType") or content.get("__typename") == "TimelineTimelineCursor":
                    continue
                promoted = _promoted(entry)
                for item in _item_contents(content):
                    results = item.get("tweet_results")
                    raw = _unwrap(results.get("result")) if isinstance(results, dict) else None
                    if raw is None:
                        continue
                    original = raw
                    reposter = None
                    retweet = (raw.get("legacy") or {}).get("retweeted_status_result")
                    if isinstance(retweet, dict):
                        candidate = _unwrap(retweet.get("result"))
                        if candidate is not None and re.fullmatch(r"\d+", str(candidate.get("rest_id", ""))):
                            original = candidate
                            reposter = _profile((raw.get("core") or {}).get("user_results", {}).get("result"))
                    ident = str(original.get("rest_id") or "")
                    if not re.fullmatch(r"\d+", ident):
                        continue
                    entries.append({
                        "id": ident,
                        "tweet": original,
                        "created_at": _timestamp((raw.get("legacy") or {}).get("created_at")),
                        "post_created_at": _timestamp((original.get("legacy") or {}).get("created_at")),
                        "reposted_by": reposter,
                        "promoted": promoted,
                    })
    return entries
