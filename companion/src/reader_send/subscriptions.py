from __future__ import annotations

import json
import os
import re
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from email.utils import parsedate_to_datetime
from pathlib import Path
from urllib.parse import urljoin, urlsplit
from xml.etree import ElementTree

from .browser_source import (AccountSubscription, archive_posts, discover_substack_subscriptions,
                             page_warnings, post_page)
from .cache import Cache
from .common import MAX_HTML_BYTES, ReaderError, atomic_write, bounded_read, digest, http_url
from .epub import build_epub
from .extract import extract
from .model import source_id
from .network import PublicHTTP, decode_html
from .sources import load_sources
from .x_browser import DEFAULT_CDP_ENDPOINT


@contextmanager
def inbox_lock(cache: Cache):
    cache.root.mkdir(parents=True, exist_ok=True)
    with (cache.root / ".subscriptions.lock").open("a+b") as lock:
        try:
            if os.name == "nt":
                import msvcrt
                lock.write(b"0")
                lock.flush()
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise ReaderError("Another subscription command is active; wait for it to finish") from exc
        try:
            yield
        finally:
            if os.name == "nt":
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(lock, fcntl.LOCK_UN)


def subscription_path(cache: Cache, name: str) -> Path:
    if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}", name):
        raise ReaderError("Publication name must be 1–64 ASCII letters/digits/hyphens/underscores")
    return cache.root / "subscriptions" / (name + ".json")


def subscriptions(cache: Cache, *, include_disabled: bool = False) -> list[dict]:
    folder = cache.root / "subscriptions"
    if not folder.exists():
        return []
    result = []
    for path in sorted(folder.glob("*.json")):
        try:
            entry = json.loads(bounded_read(path, 8192))
            if not isinstance(entry, dict) or entry.get("name") != path.stem or not (
                    entry.get("feed") or entry.get("export_dir") or entry.get("browser_url")):
                raise ValueError("invalid subscription")
        except ValueError as exc:
            raise ReaderError(f"Invalid publication configuration: {path}") from exc
        if entry.get("managed_enabled") is False and not include_disabled:
            continue
        result.append(entry)
    return result


MANAGED_ORIGIN = "substack-account"
# Publication names must start with a letter or digit, so this cannot collide with a
# per-publication skip-state file in the same folder.
REFRESH_STATE = "_account-refresh.json"


def _publication_origin(entry: dict) -> str:
    """Normalized publication origin a record subscribes to, or '' for export-only records."""
    for key in ("feed", "browser_url"):
        raw = str(entry.get(key) or "").strip()
        if not raw:
            continue
        try:
            address = urlsplit(http_url(raw))
        except ReaderError:
            continue
        return f"{address.scheme}://{address.netloc}".lower()
    return ""


def _managed_name(origin: str) -> str:
    name = re.sub(r"[^a-z0-9]+", "-", urlsplit(origin).netloc.lower()).strip("-")[:64]
    if not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}", name):
        raise ReaderError(f"Cannot derive a publication name from {origin}; add that publication manually")
    return name


def _refresh_state(cache: Cache) -> datetime | None:
    try:
        stored = json.loads(bounded_read(cache.root / "subscription-state" / REFRESH_STATE, 8192))
    except (OSError, ValueError, ReaderError):
        return None
    if not isinstance(stored, dict):
        return None
    return publication_time(str(stored.get("last_success") or ""))


def _save_refresh_state(cache: Cache, when: datetime, discovered: int) -> None:
    folder = cache.root / "subscription-state"
    folder.mkdir(parents=True, exist_ok=True)
    atomic_write(folder / REFRESH_STATE,
                 (json.dumps({"last_success": when.astimezone(timezone.utc).isoformat(timespec="seconds"),
                              "discovered": discovered}, indent=2) + "\n").encode())


def refresh_managed_subscriptions(cache: Cache, *, endpoint: str = DEFAULT_CDP_ENDPOINT,
                                  timeout: float = 30.0, now: datetime | None = None,
                                  interval: int = 86400) -> dict:
    """Reconcile account-managed publications with the signed-in Substack subscription list.

    Runs at most once per ``interval`` seconds. A failed refresh raises ``ReaderError``
    and never advances the last-success stamp or changes a record. A successful refresh
    marks matching records as account-managed, disables account-managed records that the
    account no longer lists (configuration and cached books stay), re-enables ones it
    lists again, and adds new publications as public RSS feeds unless the settings page
    itself says the publication is private/invite-only.
    """
    if interval < 0:
        raise ReaderError("Refresh interval must be zero or more seconds")
    moment = now or datetime.now(timezone.utc)
    if moment.tzinfo is None:
        moment = moment.replace(tzinfo=timezone.utc)
    last = _refresh_state(cache)
    if last is not None and moment - last < timedelta(seconds=interval):
        return {"status": "not-due", "source": MANAGED_ORIGIN, "interval": interval,
                "last_success": last.astimezone(timezone.utc).isoformat(timespec="seconds")}
    discovered = discover_substack_subscriptions(endpoint=endpoint, timeout=timeout)
    by_origin = {row.origin: row for row in discovered}
    result = {"status": "refreshed", "source": MANAGED_ORIGIN, "interval": interval,
              "discovered": len(discovered), "added": [], "updated": [], "enabled": [], "disabled": [],
              "last_success": moment.astimezone(timezone.utc).isoformat(timespec="seconds")}
    with inbox_lock(cache):
        entries = subscriptions(cache, include_disabled=True)
        known_names = {entry["name"] for entry in entries}
        existing_origins = {_publication_origin(entry) for entry in entries}
        planned: list[tuple[str, str]] = []
        for origin in by_origin:
            if origin in existing_origins:
                continue
            name = _managed_name(origin)
            if name in known_names:
                raise ReaderError(f"A publication at {origin} would be named {name}, which already names a "
                                  "different publication; add it manually instead of guessing")
            known_names.add(name)
            planned.append((origin, name))
        writes: list[tuple[Path, dict]] = []
        for entry in entries:
            origin = _publication_origin(entry)
            if not origin:
                continue  # export-only records belong to the user, never to the account list
            name = entry["name"]
            if origin in by_origin:
                wanted = {**entry, "managed_origin": MANAGED_ORIGIN, "managed_enabled": True}
                if wanted != entry:
                    writes.append((subscription_path(cache, name), wanted))
                    result["updated"].append(name)
                    if entry.get("managed_origin") == MANAGED_ORIGIN and entry.get("managed_enabled") is False:
                        result["enabled"].append(name)
            elif entry.get("managed_origin") == MANAGED_ORIGIN:
                wanted = {**entry, "managed_enabled": False}
                if wanted != entry:
                    writes.append((subscription_path(cache, name), wanted))
                    result["updated"].append(name)
                    result["disabled"].append(name)
        for origin, name in planned:
            row: AccountSubscription = by_origin[origin]
            record = {"name": name, "feed": "" if row.private else origin + "/feed",
                      "export_dir": "", "browser_url": origin + "/archive" if row.private else "",
                      "managed_origin": MANAGED_ORIGIN, "managed_enabled": True}
            writes.append((subscription_path(cache, name), record))
            result["added"].append(name)
        for path, record in writes:
            atomic_write(path, (json.dumps(record, indent=2) + "\n").encode())
        _save_refresh_state(cache, moment, len(discovered))
    return result


def add(cache: Cache, name: str, feed: str = "", export_dir: str = "", *,
        browser_url: str = "") -> dict:
    if not feed and not export_dir and not browser_url:
        raise ReaderError("Provide --feed URL, --export-dir DIRECTORY, or --browser-url URL")
    path = subscription_path(cache, name)
    if feed:
        feed = http_url(feed)
    if browser_url:
        browser_url = http_url(browser_url)
    if export_dir:
        folder = Path(export_dir).expanduser().resolve()
        if not folder.is_dir():
            raise ReaderError(f"Export directory does not exist: {folder}")
        export_dir = str(folder)
    record = {"name": name, "feed": feed, "export_dir": export_dir, "browser_url": browser_url}
    with inbox_lock(cache):
        atomic_write(path, (json.dumps(record, indent=2) + "\n").encode())
    return record


def remove(cache: Cache, name: str) -> None:
    path = subscription_path(cache, name)
    with inbox_lock(cache):
        if not path.exists():
            raise ReaderError(f"No subscription named {name}")
        path.unlink()


def feed_entries(data: bytes, feed_url: str) -> list[dict]:
    if b"<!DOCTYPE" in data.upper() or b"<!ENTITY" in data.upper():
        raise ReaderError("Feed contains unsupported DTD/entities")
    try:
        root = ElementTree.fromstring(data)
    except ElementTree.ParseError as exc:
        raise ReaderError(f"Invalid RSS/Atom feed: {exc}") from exc
    def tag(element):
        return element.tag.rsplit("}", 1)[-1]
    if tag(root) not in {"rss", "feed", "RDF"}:
        raise ReaderError("Expected an RSS or Atom feed, not a webpage")
    entries = []
    for entry in root.iter():
        if tag(entry) not in {"item", "entry"}:
            continue
        fields = {}
        link = ""
        for child in entry:
            name = tag(child)
            text = "".join(child.itertext()).strip()
            fields.setdefault(name, text)
            if name == "link" and (not child.attrib or child.attrib.get("rel", "alternate") == "alternate"):
                link = child.attrib.get("href") or text
        if not link:
            link = fields.get("guid", "") if fields.get("guid", "").startswith(("https://", "http://")) else ""
        if not link:
            continue
        url = http_url(urljoin(feed_url, link))
        entries.append({"url": url, "guid": fields.get("guid") or fields.get("id") or url,
                        "title": fields.get("title", ""), "author": fields.get("creator") or fields.get("author", ""),
                        "date": fields.get("published") or fields.get("pubDate") or fields.get("updated", "")})
    return entries


RECENT_SKIP_LIMIT = 1000


def publication_time(value: str):
    """Publication timestamp as aware UTC, or None when the source gave none."""
    text = (value or "").strip()
    if not text:
        return None
    candidate = text[:-1] + "+00:00" if text[-1] in "Zz" else text
    try:
        parsed = datetime.fromisoformat(candidate)
    except ValueError:
        try:
            parsed = parsedate_to_datetime(text)
        except (TypeError, ValueError):
            return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc)


def _skip_state(cache: Cache, name: str) -> dict[str, str]:
    """Bounded record of recent-mode skips, so an undated item is warned once and never refetched."""
    try:
        stored = json.loads(bounded_read(cache.root / "subscription-state" / (name + ".json"), 128 * 1024))
    except (OSError, ValueError):
        return {}
    if not isinstance(stored, dict) or stored.get("name") != name or not isinstance(stored.get("skipped"), dict):
        return {}
    return {str(key): str(value) for key, value in stored["skipped"].items()}


def _save_skip_state(cache: Cache, name: str, skipped: dict[str, str]) -> None:
    folder = cache.root / "subscription-state"
    folder.mkdir(parents=True, exist_ok=True)
    bounded = dict(list(skipped.items())[-RECENT_SKIP_LIMIT:])
    atomic_write(folder / (name + ".json"),
                 (json.dumps({"name": name, "skipped": bounded}, indent=2) + "\n").encode())


def sync(cache: Cache, client: PublicHTTP, *, name: str = "", limit: int = 10,
         dry_run: bool = False, no_images: bool = False,
         browser_endpoint: str = DEFAULT_CDP_ENDPOINT, timeout: float = 30.0,
         since_days: int | None = None) -> dict:
    if since_days is not None and not 1 <= since_days <= 365:
        raise ReaderError("--since-days must be between 1 and 365 days")
    cutoff = None if since_days is None else datetime.now(timezone.utc) - timedelta(days=since_days)
    configured = [s for s in subscriptions(cache) if not name or s["name"] == name]
    if not configured:
        raise ReaderError("No matching publications; add one with subscriptions add NAME --feed URL")
    result = {"items": [], "errors": [], "warnings": [], "dry_run": dry_run}
    existing = cache.entries()
    by_source = {record.get("source_id"): record for record in existing}
    known_guids = {(r.get("provenance", {}).get("feed"), r.get("provenance", {}).get("feed_guid")) for r in existing}
    browser_seen = {(r.get("provenance", {}).get("browser_url"), r.get("provenance", {}).get("browser_post_url"))
                    for r in existing}
    skips: dict[str, dict[str, str]] = {}
    originals: dict[str, dict[str, str]] = {}
    def save(item, local=None):
        article_id = cache.key(item.source_id, {"no_images": no_images})
        if dry_run:
            record = {"id": article_id, **item.metadata(), "dry_run": True}
        else:
            data = build_epub(item, article_id, client, local=local, remote_images=local is None, no_images=no_images)
            record = cache.store(article_id, item, data)
        result["items"].append(record)
        by_source[item.source_id] = record
    for publication in configured:
        label = publication["name"]
        skipped = skips.get(label)
        if skipped is None:
            skipped = _skip_state(cache, label) if cutoff is not None else {}
            skips[label] = skipped
            originals[label] = dict(skipped)
        def skip(key: str, message: str, url: str = "") -> None:
            if key in skipped:
                return
            skipped[key] = datetime.now(timezone.utc).isoformat(timespec="seconds")
            warning = {"publication": label, "warning": message}
            if url:
                warning["url"] = url
            result["warnings"].append(warning)
        # Local authorized full text wins over a public paywalled preview.
        if publication.get("export_dir"):
            try:
                for local in load_sources(Path(publication["export_dir"])):
                    metadata = local.metadata
                    item = extract(decode_html(local.data), metadata.get("source_url", ""), title=metadata.get("title", ""),
                                   author=metadata.get("author", ""), date=metadata.get("date", ""))
                    item.source_type = "substack-export"
                    if not item.source_url:
                        item.source_id = "local:" + str(local.path or local.archive) + "#" + local.member
                    item.provenance = {"publication": label, "content_hash": digest(local.data), "input": local.name}
                    old = by_source.get(item.source_id)
                    if old and old.get("provenance", {}).get("content_hash") == item.provenance["content_hash"]:
                        continue
                    save(item, local)
            except (ReaderError, OSError) as exc:
                result["errors"].append({"publication": label, "stage": "local-export", "error": str(exc)})
        browser_url = publication.get("browser_url") or ""
        if browser_url:
            # Publications that disable RSS still render their archive in the browser.
            try:
                links = archive_posts(browser_url, endpoint=browser_endpoint, timeout=timeout)
            except (ReaderError, OSError) as exc:
                result["errors"].append({"publication": label, "stage": "browser", "error": str(exc)})
            else:
                candidates = []
                for link in links:
                    when = publication_time(link.get("date", ""))
                    if cutoff is not None:
                        if when is not None and when < cutoff:
                            continue
                        if when is None and f"browser:{browser_url}|{link['url']}" in skipped:
                            continue
                    candidates.append(link)
                new = [link for link in candidates if source_id(link["url"]) not in by_source
                       and (browser_url, link["url"]) not in browser_seen][:limit]
                for link in reversed(new):
                    try:
                        page = post_page(link["url"], endpoint=browser_endpoint, timeout=timeout)
                        item = extract(page.html, page.href)
                        when = publication_time(item.date) or publication_time(link.get("date", ""))
                        if cutoff is not None and (when is None or when < cutoff):
                            key = f"browser:{browser_url}|{link['url']}"
                            if when is None:
                                skip(key, "Skipped a post with no publication date; recent mode never "
                                          "assumes today", link["url"])
                            else:
                                skipped.setdefault(key, datetime.now(timezone.utc).isoformat(timespec="seconds"))
                            continue
                        if item.title == "Untitled article" and link["text"]:
                            item.title = link["text"]
                        if item.source_id in by_source:
                            browser_seen.add((browser_url, link["url"]))
                            continue
                        item.source_type = "substack-browser" if "substack" in browser_url else "browser"
                        item.provenance = {"publication": label, "browser_url": browser_url,
                                           "browser_post_url": page.href}
                        item.warnings.extend(page_warnings(page.text))
                        save(item)
                        browser_seen.add((browser_url, link["url"]))
                    except (ReaderError, OSError) as exc:
                        result["errors"].append({"publication": label, "stage": "browser-post",
                                                 "url": link["url"], "error": str(exc)})
        if not publication.get("feed"):
            continue
        feed = publication["feed"]
        try:
            entries = feed_entries(client.get(feed, MAX_HTML_BYTES).data, feed)
        except (ReaderError, OSError) as exc:
            result["errors"].append({"publication": label, "stage": "feed", "error": str(exc)})
            continue
        if cutoff is not None:
            recent_entries = []
            for entry in entries:
                when = publication_time(entry.get("date", ""))
                if when is None:
                    skip(f"rss:{feed}|{entry['guid']}",
                         "Skipped an undated feed entry; recent mode never assumes a publication date",
                         entry["url"])
                    continue
                if when >= cutoff:
                    recent_entries.append(entry)
            entries = recent_entries
        new = [e for e in entries if source_id(e["url"]) not in by_source and (feed, e["guid"]) not in known_guids][:limit]
        for entry in reversed(new):
            try:
                response = client.get(entry["url"])
                item = extract(decode_html(response.data, response.charset), response.url)
                if item.title == "Untitled article" and entry["title"]:
                    item.title = entry["title"]
                item.author = item.author or entry["author"]
                item.date = item.date or entry["date"]
                if item.source_id in by_source:
                    continue
                item.source_type = "substack" if "substack" in feed else "rss"
                item.provenance = {"publication": label, "feed": feed, "feed_guid": entry["guid"]}
                save(item)
                known_guids.add((feed, entry["guid"]))
            except (ReaderError, OSError) as exc:
                result["errors"].append({"publication": label, "url": entry["url"], "error": str(exc)})
    if cutoff is not None and not dry_run:
        for label, skipped in skips.items():
            if skipped != originals.get(label, {}):
                _save_skip_state(cache, label, skipped)
    return result
