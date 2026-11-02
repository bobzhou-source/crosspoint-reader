"""Read a publication's visible archive and posts from the signed-in Chrome session.

Some publications disable RSS even for signed-in subscribers while still rendering
their posts in the browser. This module covers that case with read-only navigation:
a fresh tab is opened at the chosen archive URL, the visible same-origin ``/p/…``
links are read from the rendered page, and each new post is opened in turn so the
rendered HTML can go through the existing extractor/EPUB pipeline.

Only what Chrome already renders is used. Cookies, Authorization headers, local
storage and other tabs are never read, persisted or forwarded; the module never
launches a profile or signs in. Transport comes from ``x_browser`` so there is one
DevTools client in the project.
"""

from __future__ import annotations

import re
import time
from contextlib import contextmanager
from dataclasses import dataclass
from urllib.parse import urlsplit

from .common import MAX_HTML_BYTES, ReaderError, http_url
from .x_browser import (DEFAULT_CDP_ENDPOINT, _Session, _Timeout, _close_target,
                        _devtools, _new_target, cdp_endpoint)

# Runtime.evaluate results travel JSON-escaped inside an 8 MiB websocket frame, so a
# page snapshot is capped in characters rather than bytes.
BROWSER_PAGE_CHARS = 4 * 1024 * 1024
# One archive page cannot make the scan unbounded; newest links come first.
ARCHIVE_LINKS = 200
POST_PATH = re.compile(r"^/p/[^/]+/?$")
INVITE_ONLY = re.compile(r"this publication is invite-only", re.I)
# The settings page lists a bounded first page of publications and one ordinary
# disclosure button ("Show N more"); expanding it is read-only.
SETTINGS_URL = "https://substack.com/settings"
ACCOUNT_ROWS = 500
SHOW_MORE = re.compile(r"^show (\d+) more$", re.I)
MEMBERSHIP = re.compile(r"\s+(?:free|paid|founding|comped|student|group)\s+subscriber\s*$", re.I)
# Only wording the account UI itself uses for a withheld publication. A public feed
# failing anonymously is never such a statement.
PRIVATE_LABEL = re.compile(r"\b(?:private|invite[- ]only)\b", re.I)
SIGNED_OUT = re.compile(r"\b(?:sign|log) in\b|\benter your email\b", re.I)
SIGN_IN_PATH = re.compile(r"^/(?:log-?in|sign-?in)(?:/|$)", re.I)
# Only statements that appear when the post body is actually withheld. Generic header
# upsells ("Upgrade to paid", "Subscribe to continue reading") show on every page for a
# free subscriber and must never be read as a paywall.
PARTIAL = re.compile(r"this post is (?:for|only for) (?:paid|paying) subscribers only|"
                     r"this post is only available to (?:paid|paying) subscribers|"
                     r"(?:the rest of this post|the remainder of this post) is for "
                     r"(?:paid|paying) subscribers", re.I)

ARCHIVE_JS = r"""(() => {
  const origin = location.origin;
  const seen = new Map();
  const stamp = (node) => {
    let level = 0;
    for (let element = node; element && element !== document.body && level < 4;
         element = element.parentElement, level++) {
      const time = element.querySelector('time[datetime]');
      if (time) return time.getAttribute('datetime') || '';
    }
    return '';
  };
  for (const link of document.querySelectorAll('a[href]')) {
    let address = null;
    try { address = new URL(link.getAttribute('href') || '', location.href); } catch (error) { continue; }
    if (address.origin !== origin) continue;
    const path = address.pathname.replace(/\/$/, '');
    if (!/^\/p\/[^\/]+$/.test(path)) continue;
    const clean = origin + path;
    if (!seen.has(clean)) {
      seen.set(clean, {text: (link.innerText || link.textContent || '').replace(/\s+/g, ' ').trim().slice(0, 300),
                       date: stamp(link).slice(0, 64)});
    }
    if (seen.size >= %d) break;
  }
  return {href: location.href, title: document.title,
          body: (document.body ? document.body.innerText : '').replace(/\s+/g, ' ').slice(0, 600),
          links: [...seen.entries()].map(([url, meta]) => ({url: url, text: meta.text, date: meta.date}))};
})()""" % ARCHIVE_LINKS

PAGE_JS = r"""(() => {
  const root = document.documentElement;
  const html = root ? root.outerHTML : '';
  return {href: location.href, title: document.title, length: html.length,
          text: (document.body ? document.body.innerText : '').replace(/\s+/g, ' ').slice(0, 4000),
          html: html.length <= %d ? html : null};
})()""" % BROWSER_PAGE_CHARS

ACCOUNT_JS = r"""(() => {
  const visible = (node) => !!(node.offsetWidth || node.offsetHeight || node.getClientRects().length);
  const clean = (value, limit) => (value || '').replace(/\s+/g, ' ').trim().slice(0, limit);
  const rows = [];
  for (const link of document.querySelectorAll('a[href]')) {
    if (rows.length >= %d) break;
    if (!visible(link)) continue;
    let address = null;
    try { address = new URL(link.getAttribute('href') || '', location.href); } catch (error) { continue; }
    if (address.pathname.replace(/\/$/, '') !== '/account') continue;
    rows.push({url: address.href, text: clean(link.innerText || link.textContent, 200)});
  }
  const more = [...document.querySelectorAll('button')].filter(visible)
      .map((button) => clean(button.innerText || button.textContent, 40))
      .find((text) => /^Show \d+ more$/.test(text)) || '';
  return {href: location.href, title: document.title, more: more,
          body: (document.body ? document.body.innerText : '').replace(/\s+/g, ' ').slice(0, 2000),
          rows: rows};
})()""" % ACCOUNT_ROWS


def _expand_js(more: int) -> str:
    return r"""(() => {
  const visible = (node) => !!(node.offsetWidth || node.offsetHeight || node.getClientRects().length);
  const label = (node) => (node.innerText || node.textContent || '').replace(/\s+/g, ' ').trim();
  const button = [...document.querySelectorAll('button')].filter(visible)
      .find((node) => label(node) === 'Show %d more');
  if (!button) return false;
  button.click();
  return true;
})()""" % more


@dataclass
class BrowserPage:
    href: str
    html: str
    text: str


@dataclass(frozen=True)
class AccountSubscription:
    """One publication the signed-in account's settings page lists."""
    url: str
    origin: str
    name: str
    # True only when the row itself says private/invite-only; the page never states it
    # for ordinary free subscriptions.
    private: bool


def _probe(origin: str, timeout: float) -> None:
    try:
        _devtools(origin, min(timeout, 5.0))
    except ReaderError as exc:
        raise ReaderError(f"{exc}. The browser source only reuses Chrome that is already running and signed in; "
                          "it never launches a profile, reads cookies, or signs in") from None


@contextmanager
def _tab(url: str, *, endpoint: str, timeout: float):
    """Open one fresh tab at url, wait for it to finish loading, yield its live session."""
    address = http_url(url)
    origin = cdp_endpoint(endpoint)
    budget = max(1.0, min(float(timeout), 300.0))
    deadline = time.monotonic() + budget
    _probe(origin, budget)
    target = _new_target(origin, min(budget, 10.0))
    session = None
    try:
        session = _Session(str(target["webSocketDebuggerUrl"]), budget)
        session.call("Page.enable", {}, deadline)
        navigate = session.call("Page.navigate", {"url": address}, deadline)
        if navigate.get("errorText"):
            raise ReaderError(f"Chrome could not open {address}: {navigate['errorText']}")
        while True:
            try:
                ready = session.call("Runtime.evaluate",
                                     {"expression": "document.readyState === 'complete'", "returnByValue": True},
                                     deadline).get("result") or {}
            except ReaderError:
                ready = {}
            if ready.get("value") is True:
                break
            if time.monotonic() >= deadline:
                raise _Timeout
            time.sleep(0.1)
        yield session, deadline, address, budget
    except _Timeout:
        raise ReaderError(f"Chrome did not finish rendering {address} within {budget:g}s. Keep that Chrome window "
                          "free and the connection up, then retry") from None
    finally:
        if session is not None:
            session.close()
        try:
            _close_target(origin, str(target["id"]), min(budget, 5.0))
        except ReaderError:
            pass


def _evaluate(url: str, expression: str, *, endpoint: str, timeout: float):
    """Open one fresh tab at url, wait for it to finish loading, return expression's value."""
    with _tab(url, endpoint=endpoint, timeout=timeout) as (session, deadline, _address, _budget):
        result = session.call("Runtime.evaluate", {"expression": expression, "returnByValue": True}, deadline)
        return (result.get("result") or {}).get("value")


def parse_archive(value, browser_url: str) -> list[dict]:
    """Keep only visible same-origin post links from one rendered archive page."""
    if not isinstance(value, dict):
        raise ReaderError("Chrome returned no readable archive page; open that URL in the Chrome window and retry")
    origin = urlsplit(http_url(browser_url))
    base = f"{origin.scheme}://{origin.netloc}"
    found: list[dict] = []
    seen: set[str] = set()
    for link in value.get("links") or []:
        if not isinstance(link, dict):
            continue
        raw = str(link.get("url") or "")
        try:
            address = urlsplit(http_url(raw))
        except ReaderError:
            continue
        if f"{address.scheme}://{address.netloc}" != base or not POST_PATH.match(address.path):
            continue
        clean = base + address.path.rstrip("/")
        if clean in seen:
            continue
        seen.add(clean)
        found.append({"url": clean, "text": str(link.get("text") or "").strip()[:300],
                      "date": str(link.get("date") or "").strip()[:64]})
        if len(found) >= ARCHIVE_LINKS:
            break
    if not found:
        path = urlsplit(str(value.get("href") or "")).path
        if SIGN_IN_PATH.match(path) or path.rstrip("/") == "/welcome":
            raise ReaderError(f"Chrome was redirected from {browser_url} to {path}: this browser profile is not "
                              "signed in to Substack or not subscribed to this publication. Sign in once in that "
                              "Chrome profile and retry; nothing is guessed or bypassed")
        raise ReaderError(f"Chrome rendered {browser_url} but it shows no same-origin post links. Open the "
                          "publication archive in that signed-in Chrome window, or check that this account can "
                          "see the posts; nothing is guessed or bypassed")
    return found


def account_rows(value) -> list[AccountSubscription]:
    """Publications listed on one rendered settings page, one per publication origin."""
    if not isinstance(value, dict):
        raise ReaderError(f"Chrome returned no readable Substack settings page; open {SETTINGS_URL} in that "
                          "signed-in Chrome window and retry")
    found: list[AccountSubscription] = []
    seen: set[str] = set()
    for row in value.get("rows") or []:
        if not isinstance(row, dict):
            continue
        raw = str(row.get("url") or "")
        try:
            address = urlsplit(http_url(raw))
        except ReaderError:
            continue
        if address.path.rstrip("/") != "/account":
            continue
        origin = f"{address.scheme}://{address.netloc}".lower()
        if origin in seen:
            raise ReaderError(f"Substack settings lists {origin} twice; the subscription list is ambiguous, so "
                              "nothing was changed")
        seen.add(origin)
        text = " ".join(str(row.get("text") or "").split())[:200]
        name = MEMBERSHIP.sub("", text).strip() or address.netloc
        found.append(AccountSubscription(url=http_url(raw), origin=origin, name=name[:200],
                                         private=bool(PRIVATE_LABEL.search(text))))
        if len(found) >= ACCOUNT_ROWS:
            break
    return found


def account_expander(value) -> int | None:
    """Number of still-hidden rows behind "Show N more", or None when nothing is hidden."""
    if not isinstance(value, dict):
        return None
    match = SHOW_MORE.match(" ".join(str(value.get("more") or "").split()))
    return int(match.group(1)) if match else None


def _account_problem(value, address: str, message: str) -> ReaderError:
    value = value if isinstance(value, dict) else {}
    hint = (" Sign in to Substack once in that Chrome window, then retry" if SIGNED_OUT.search(
        str(value.get("body") or "")) or SIGN_IN_PATH.match(urlsplit(str(value.get("href") or "")).path)
        else " Open that page while signed in and retry")
    return ReaderError(f"{message}.{hint}; nothing was changed")


def discover_substack_subscriptions(*, endpoint: str = DEFAULT_CDP_ENDPOINT, timeout: float = 30.0,
                                    settings_url: str = SETTINGS_URL) -> list[AccountSubscription]:
    """The complete publication list from the signed-in settings page, or nothing.

    The page renders after hydration and discloses hidden rows with one ordinary
    "Show N more" button. The list counts as complete only when every disclosure
    settles to the announced total with no button left; otherwise this raises and
    the caller must not touch any configuration.
    """
    with _tab(settings_url, endpoint=endpoint, timeout=timeout) as (session, deadline, address, _budget):
        def snapshot():
            result = session.call("Runtime.evaluate", {"expression": ACCOUNT_JS, "returnByValue": True}, deadline)
            return (result.get("result") or {}).get("value")

        value = snapshot()
        while not account_rows(value):
            if time.monotonic() >= deadline:
                raise _account_problem(value, address, f"Chrome rendered {address} but shows no subscription rows")
            time.sleep(0.25)
            try:
                value = snapshot()
            except _Timeout:
                raise _account_problem(value, address,
                                       f"Chrome rendered {address} but shows no subscription rows") from None
        expected = len(account_rows(value))
        for _ in range(5):
            more = account_expander(value)
            if more is None:
                break
            expected += more
            clicked = session.call("Runtime.evaluate", {"expression": _expand_js(more),
                                                        "returnByValue": True}, deadline)
            if (clicked.get("result") or {}).get("value") is not True:
                raise ReaderError(f"Substack settings offers 'Show {more} more' but Chrome could not activate it, "
                                  "so the subscription list is incomplete; nothing was changed")
            while True:
                value = snapshot()
                if len(account_rows(value)) >= expected and account_expander(value) is None:
                    break
                if time.monotonic() >= deadline:
                    raise _account_problem(value, address, "Substack settings did not finish expanding its "
                                                           "subscription list")
                time.sleep(0.25)
        else:
            raise ReaderError("Substack settings kept disclosing more subscriptions; the list did not settle, "
                              "so nothing was changed")
        rows = account_rows(value)
        more = account_expander(value)
        if more is not None:
            raise ReaderError(f"Substack settings still hides {more} subscriptions behind another disclosure, "
                              "so the list is incomplete; nothing was changed")
        if len(rows) != expected:
            raise ReaderError(f"Substack settings announced {expected} subscriptions but showed {len(rows)}, "
                              "so the list is incomplete; nothing was changed")
        return rows


def page_from_value(value, url: str) -> BrowserPage:
    """Validate one rendered post snapshot; refuse oversized or gated pages."""
    if not isinstance(value, dict):
        raise ReaderError(f"Chrome returned no readable page for {url}")
    page = value.get("html")
    if not isinstance(page, str) or not page:
        raise ReaderError(f"Chrome rendered {url} at {value.get('length')} characters, above the "
                          f"{BROWSER_PAGE_CHARS // (1024 * 1024)} MiB browser-capture bound; save that post as HTML "
                          "and use an export directory instead")
    if len(page) > MAX_HTML_BYTES:
        raise ReaderError(f"Chrome rendered {url} above the {MAX_HTML_BYTES // (1024 * 1024)} MiB HTML bound; "
                          "save that post as HTML and use an export directory instead")
    text = str(value.get("text") or "")
    if INVITE_ONLY.search(text):
        raise ReaderError(f"Chrome shows an invite-only notice for {url}: this browser session is not signed in to "
                          "or not authorized for this publication. Sign in to it once in the Chrome window on the "
                          "DevTools port and retry; nothing is bypassed")
    href = http_url(str(value.get("href") or url))
    return BrowserPage(href=href, html=page, text=text)


def archive_posts(browser_url: str, *, endpoint: str = DEFAULT_CDP_ENDPOINT, timeout: float = 30.0) -> list[dict]:
    """Visible posts on the publication's archive page, newest first as rendered."""
    value = _evaluate(browser_url, ARCHIVE_JS, endpoint=endpoint, timeout=timeout)
    return parse_archive(value, browser_url)


def post_page(url: str, *, endpoint: str = DEFAULT_CDP_ENDPOINT, timeout: float = 30.0) -> BrowserPage:
    """Rendered HTML for one visible post, from the already signed-in browser."""
    return page_from_value(_evaluate(url, PAGE_JS, endpoint=endpoint, timeout=timeout), url)


def page_warnings(page_text: str) -> list[str]:
    """Visible-only content warnings; never a reason to fetch or bypass anything."""
    if PARTIAL.search(page_text):
        return ["This post is partly paywalled; the EPUB contains only the portion this account can read."]
    return []
