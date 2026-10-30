from __future__ import annotations

import html
import json
import os
import re
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

from .common import MAX_HTML_BYTES, USER_AGENT, ReaderError, clean_text, http_url
from .extract import extract
from .model import ReadingItem


TWEET_FIELDS = "id,text,author_id,created_at,conversation_id,referenced_tweets,entities,attachments,note_tweet"
EXPANSIONS = "author_id,attachments.media_keys,referenced_tweets.id,referenced_tweets.id.author_id"


def post_id(url: str) -> str:
    parts = urlsplit(http_url(url))
    if parts.hostname not in {"x.com", "www.x.com", "twitter.com", "www.twitter.com", "mobile.twitter.com"}:
        raise ReaderError("X adapter requires an x.com/twitter.com post URL")
    match = re.fullmatch(r"/(?:[A-Za-z0-9_]+/status|i/web/status)/(\d+)(?:/.*)?", parts.path)
    if not match:
        raise ReaderError("Provide a specific X post/thread URL, not a profile or search URL")
    return match.group(1)


def load_export(data: bytes) -> dict:
    try:
        payload = json.loads(data)
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise ReaderError("X export must be JSON in official API v2 data/includes format") from exc
    if isinstance(payload, dict) and "data" not in payload and isinstance(payload.get("meta"), dict) and payload["meta"].get("result_count") == 0:
        payload["data"] = []
    if not isinstance(payload, dict) or not isinstance(payload.get("data"), (dict, list)):
        raise ReaderError("X export requires {data: [tweets], includes: {users, media, tweets}}")
    return payload


def thread_item(url: str, payload: dict) -> ReadingItem:
    requested = post_id(url)
    data = payload.get("data", [])
    data = [data] if isinstance(data, dict) else data
    includes = payload.get("includes", {})
    if (not isinstance(data, list) or not isinstance(includes, dict)
            or any(not isinstance(includes.get(key, []), list) for key in ("tweets", "users", "media"))):
        raise ReaderError("Malformed X data/includes export")
    tweets = {}
    for tweet in includes.get("tweets", []) + data:
        if isinstance(tweet, dict) and re.fullmatch(r"\d+", str(tweet.get("id", ""))):
            for key in ("entities", "attachments", "note_tweet"):
                if key in tweet and not isinstance(tweet[key], dict):
                    raise ReaderError(f"Malformed X post {tweet['id']}: {key} must be an object")
            if not isinstance(tweet.get("referenced_tweets", []), list):
                raise ReaderError("X referenced_tweets must be a list")
            tweets[str(tweet["id"])] = tweet
    if requested not in tweets:
        raise ReaderError("The requested X post is missing from this export/API response")
    target = tweets[requested]
    author_id = str(target.get("author_id", ""))
    if not author_id:
        raise ReaderError("X posts need author_id to distinguish the author's thread from replies")
    users = {str(user.get("id")): user for user in includes.get("users", []) if isinstance(user, dict)}
    media = {str(entry.get("media_key")): entry for entry in includes.get("media", []) if isinstance(entry, dict)}
    def references(tweet, kind):
        return [str(ref.get("id")) for ref in tweet.get("referenced_tweets", [])
                if isinstance(ref, dict) and ref.get("type") == kind and re.fullmatch(r"\d+", str(ref.get("id", "")))]
    root_id = requested
    visited = set()
    while root_id not in visited:
        visited.add(root_id)
        parents = references(tweets[root_id], "replied_to")
        parent = tweets.get(parents[0]) if parents else None
        if not parent or str(parent.get("author_id")) != author_id:
            break
        root_id = str(parent["id"])
        if root_id in visited:
            raise ReaderError("Cyclic X reply graph")
    connected = {root_id}
    ordered = [tweets[root_id]]
    while True:
        children = [tweet for ident, tweet in tweets.items() if ident not in connected and str(tweet.get("author_id")) == author_id
                    and any(parent in connected for parent in references(tweet, "replied_to"))]
        if not children:
            break
        children.sort(key=lambda tweet: int(tweet["id"]))
        for tweet in children:
            connected.add(str(tweet["id"]))
            ordered.append(tweet)
    if requested not in connected:
        raise ReaderError("Requested post is not in a valid connected author thread")
    ordered.sort(key=lambda tweet: int(tweet["id"]))
    person = users.get(author_id, {})
    username = str(person.get("username") or "i/web")
    if not re.fullmatch(r"[A-Za-z0-9_]+", username):
        username = "i/web"
    author = clean_text(str(person.get("name") or username))
    if username != "i/web":
        author += " (@" + username + ")"
    esc = html.escape
    def link(tweet):
        user = users.get(str(tweet.get("author_id")), {})
        handle = str(user.get("username", "i/web"))
        handle = handle if re.fullmatch(r"[A-Za-z0-9_]+", handle) else "i/web"
        return f"https://x.com/{handle}/status/{tweet['id']}"
    def text_html(tweet):
        note = tweet.get("note_tweet") or {}
        text = str(note.get("text") or tweet.get("text") or "")
        entities = note.get("entities") or tweet.get("entities") or {}
        urls = entities.get("urls", []) if isinstance(entities, dict) else []
        # Replace by literal URL rather than UTF-16 entity offsets.
        rendered = esc(text)
        for entity in urls:
            if not isinstance(entity, dict):
                continue
            short = entity.get("url", "")
            expanded = entity.get("unwound_url") or entity.get("expanded_url") or short
            try:
                expanded = http_url(expanded)
            except ReaderError:
                continue
            if short:
                rendered = rendered.replace(esc(short), f'<a href="{esc(expanded, quote=True)}">{esc(expanded)}</a>')
        return "".join(f"<p>{line}</p>" for line in rendered.splitlines() if line.strip())
    body = []
    preview_urls = []
    preview_url_set = set()
    def remember_media_link(value):
        if value not in preview_url_set:
            preview_url_set.add(value)
            preview_urls.append(value)
    for index, tweet in enumerate(ordered, 1):
        body.append(f'<h2>Post {index}</h2><p>{esc(str(tweet.get("created_at", "")))} · <a href="{link(tweet)}">Original post</a></p>')
        body.append(text_html(tweet))
        for quoted_id in references(tweet, "quoted"):
            quoted = tweets.get(quoted_id)
            if quoted:
                quoted_author = users.get(str(quoted.get("author_id")), {}).get("name", "Quoted author")
                body.append(f'<blockquote><p>{esc(str(quoted_author))} · <a href="{link(quoted)}">Quoted post</a></p>{text_html(quoted)}</blockquote>')
            else:
                body.append(f'<blockquote><a href="https://x.com/i/web/status/{quoted_id}">Quoted post (not included in export/access)</a></blockquote>')
        attachments = tweet.get("attachments", {})
        for key in attachments.get("media_keys", []) if isinstance(attachments, dict) else []:
            entry = media.get(str(key), {})
            preview = entry.get("url") if entry.get("type") == "photo" else entry.get("preview_image_url")
            if preview:
                try:
                    preview = http_url(preview)
                    body.append(f'<figure><img src="{esc(preview, quote=True)}" alt="{esc(str(entry.get("alt_text") or "Post media preview"), quote=True)}"/></figure>')
                    if entry.get("type") == "photo":
                        remember_media_link(preview)
                except ReaderError:
                    pass
            if entry.get("type") in {"video", "animated_gif"}:
                body.append(f'<p>Video preview only — <a href="{link(tweet)}">watch the original online</a>.</p>')
                remember_media_link(link(tweet))
        entities = tweet.get("entities") or {}
        for entry in entities.get("urls", []) if isinstance(entities, dict) else []:
            expanded = str(entry.get("expanded_url", "")) if isinstance(entry, dict) else ""
            if "/i/spaces/" in expanded:
                try:
                    expanded = http_url(expanded)
                    body.append(f'<p>Spaces audio is not embedded. <a href="{esc(expanded, quote=True)}">Open the original Space</a>.</p>')
                    remember_media_link(expanded)
                except ReaderError:
                    pass
    first_text = str((ordered[0].get("note_tweet") or {}).get("text") or ordered[0].get("text", ""))
    title = clean_text(first_text)[:120] or "X thread"
    canonical = link(ordered[0])
    item = extract("<article>" + "".join(body) + "</article>", canonical, title=title, author=author,
                   date=str(ordered[0].get("created_at", "")))
    item.source_type = "x"
    item.source_id = "x:" + str(target.get("conversation_id") or root_id) + ":" + author_id
    item.provenance = {"requested_url": http_url(url), "conversation_id": str(target.get("conversation_id") or root_id),
                       "post_ids": ",".join(str(tweet["id"]) for tweet in ordered),
                       "media_links": json.dumps(preview_urls), "qr_payload": canonical}
    item.warnings.append("Thread includes only accessible/exported connected posts by this author; missing/deleted posts cannot be reconstructed.")
    if payload.get("_truncated"):
        item.warnings.append("API page limit reached; export the full thread for complete coverage.")
    return item


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        raise ReaderError("X API redirected; refusing to forward the bearer token")


def api_thread(url: str, timeout: float = 30, max_pages: int = 3) -> dict:
    ident = post_id(url)
    token = os.environ.get("X_BEARER_TOKEN", "")
    if not token or any(c.isspace() for c in token):
        raise ReaderError("--api requires your authorized X_BEARER_TOKEN environment variable; alternatively use --export THREAD.json")
    opener = build_opener(NoRedirect())
    def get(route, params):
        request = Request("https://api.x.com/2/" + route + "?" + urlencode(params),
                          headers={"Authorization": "Bearer " + token, "User-Agent": USER_AGENT, "Accept": "application/json"})
        try:
            with opener.open(request, timeout=timeout) as response:
                data = response.read(MAX_HTML_BYTES + 1)
                if len(data) > MAX_HTML_BYTES:
                    raise ReaderError("X API response exceeds 8 MiB")
                return load_export(data)
        except HTTPError as exc:
            raise ReaderError(f"X API HTTP {exc.code}; verify your authorized API plan, post access and quota, or use --export. No fallback scraping was attempted.") from exc
        except (URLError, OSError) as exc:
            raise ReaderError(f"X API request failed: {exc}") from exc
    fields = {"tweet.fields": TWEET_FIELDS, "expansions": EXPANSIONS,
              "user.fields": "id,name,username", "media.fields": "media_key,type,url,preview_image_url,alt_text"}
    initial = get("tweets/" + ident, fields)
    target = initial["data"]
    if not isinstance(target, dict) or not target.get("author_id") or not target.get("conversation_id"):
        raise ReaderError("X API did not return the requested post author/conversation")
    payload = {"data": [target], "includes": {key: list(initial.get("includes", {}).get(key, [])) for key in ("users", "tweets", "media")}}
    params = {**fields, "query": f"conversation_id:{target['conversation_id']} from:{target['author_id']}", "max_results": 100}
    for _ in range(max_pages):
        page = get("tweets/search/recent", params)
        page_data = page["data"] if isinstance(page["data"], list) else [page["data"]]
        payload["data"].extend(page_data)
        for key in payload["includes"]:
            payload["includes"][key].extend(page.get("includes", {}).get(key, []))
        next_token = page.get("meta", {}).get("next_token")
        if not next_token:
            return payload
        params["next_token"] = next_token
    payload["_truncated"] = True
    return payload
