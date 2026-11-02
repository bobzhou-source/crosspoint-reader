"""Collect the newest posts from the X Following timeline into the reading cache.

Each pass reads what X renders for the real Following tab through the already signed-in
Chrome window, keeps only posts inside ``--since-hours`` that have not been stored yet,
and writes ordinary cache EPUBs, so every existing delivery route (card sync, LocalSend,
inbox) works unchanged. Ads, promoted injections and posts by accounts that only appear as
replies inside a conversation module are never turned into books, and no thread is claimed
or reconstructed from a timeline batch.
"""

from __future__ import annotations

import json
import os
import re
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path

from .cache import Cache
from .common import ReaderError, atomic_write, bounded_read
from .epub import build_epub
from .network import PublicHTTP
from .x_adapter import thread_item
from .x_browser import DEFAULT_CDP_ENDPOINT, api_payload, capture_following_feed, feed_entries

SEEN_LIMIT = 5000


def state_path(cache: Cache) -> Path:
    return cache.root / "x-following.json"


@contextmanager
def lock(cache: Cache):
    """Serialize collector passes without touching the Substack inbox lock."""
    cache.root.mkdir(parents=True, exist_ok=True)
    with (cache.root / ".x-following.lock").open("a+b") as handle:
        try:
            if os.name == "nt":
                import msvcrt
                handle.write(b"0")
                handle.flush()
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise ReaderError("Another x-following sync is active; wait for it to finish") from exc
        try:
            yield
        finally:
            if os.name == "nt":
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle, fcntl.LOCK_UN)


def load_state(cache: Cache) -> dict:
    path = state_path(cache)
    if not path.exists():
        return {"seen": [], "last_run": "", "last_counts": {}}
    try:
        state = json.loads(bounded_read(path, 512 * 1024))
        if not isinstance(state, dict) or not isinstance(state.get("seen", []), list):
            raise ValueError("invalid state")
    except (ValueError, UnicodeDecodeError) as exc:
        raise ReaderError(f"Corrupt X Following state: {path}; delete it to rescan the last window") from exc
    return {"seen": [value for value in (str(item) for item in state.get("seen", []))
                     if re.fullmatch(r"\d+", value)][:SEEN_LIMIT],
            "last_run": str(state.get("last_run", "")),
            "last_counts": state.get("last_counts") if isinstance(state.get("last_counts"), dict) else {}}


def save_state(cache: Cache, state: dict) -> None:
    atomic_write(state_path(cache), (json.dumps(state, indent=2) + "\n").encode())


def sync(cache: Cache, client: PublicHTTP, *, since_hours: int = 24, limit: int = 50,
         dry_run: bool = False, no_images: bool = False, endpoint: str = DEFAULT_CDP_ENDPOINT,
         timeout: float = 45.0, deliver=None) -> dict:
    """Read the Following timeline once and store new posts inside the time window."""
    if not 1 <= since_hours <= 168:
        raise ReaderError("--since-hours must be between 1 and 168 hours")
    if not 1 <= limit <= 200:
        raise ReaderError("--limit must be between 1 and 200 posts")
    with lock(cache):
        state = load_state(cache)
        seen = state["seen"]
        seen_set = set(seen)
        cutoff = datetime.now(timezone.utc) - timedelta(hours=since_hours)
        result = {"collected": 0, "created": 0, "cached": 0, "skipped_seen": 0, "skipped_window": 0,
                  "skipped_ads": 0, "skipped_duplicate": 0, "undated": 0, "truncated": False,
                  "window_gap": False, "oldest_post": "", "items": [], "errors": [], "warnings": []}
        try:
            payloads = capture_following_feed(endpoint=endpoint, timeout=timeout)
        except (ReaderError, OSError) as exc:
            result["errors"].append({"stage": "browser", "error": str(exc)})
            result["warnings"].append("Nothing was stored because the Following timeline could not be read.")
            return result
        entries = feed_entries(payloads)
        result["collected"] = len(entries)
        result["oldest_post"] = min((entry["created_at"] for entry in entries
                                     if entry["created_at"] is not None and not entry["promoted"]),
                                    default=datetime.now(timezone.utc)).isoformat(timespec="seconds")
        eligible: list[dict] = []
        chosen: set[str] = set()
        for entry in entries:
            if entry["promoted"]:
                result["skipped_ads"] += 1
                continue
            if entry["created_at"] is None:
                result["undated"] += 1
                continue
            if entry["created_at"] < cutoff:
                result["skipped_window"] += 1
                continue
            if entry["id"] in seen_set:
                result["skipped_seen"] += 1
                continue
            if entry["id"] in chosen:
                result["skipped_duplicate"] += 1
                continue
            chosen.add(entry["id"])
            eligible.append(entry)
        result["window_gap"] = bool(eligible) and min(entry["created_at"] for entry in eligible) > cutoff
        result["truncated"] = len(eligible) > limit
        stored: list[str] = []
        for entry in eligible[:limit]:
            ident = entry["id"]
            payload = api_payload(payloads, ident, include_thread=False)
            if payload is None:
                result["errors"].append({"post": ident, "stage": "convert",
                                         "error": "post is missing from the captured timeline batch"})
                continue
            try:
                item = thread_item(f"https://x.com/i/web/status/{ident}", payload, thread=False)
            except ReaderError as exc:
                result["errors"].append({"post": ident, "stage": "convert", "error": str(exc)})
                continue
            item.provenance.update({"capture": "chrome-cdp", "feed": "following", "timeline_post": ident})
            reposter = entry["reposted_by"] or {}
            if reposter:
                handle = str(reposter.get("username") or "")
                label = str(reposter.get("name") or handle or "an account you follow")
                item.provenance["reposted_by"] = label + (f" (@{handle})" if handle else "")
            article_id = cache.key(item.source_id, {"no_images": no_images})
            if dry_run:
                result["items"].append({"id": article_id, "title": item.title, "path": "",
                                        "source_url": item.source_url, "post_id": ident, "dry_run": True})
                result["created"] += 1
                continue
            try:
                existing = cache.get(article_id, verify=False)
            except ReaderError:
                existing = None
            if existing is not None:
                stored.append(ident)
                result["cached"] += 1
                result["items"].append({"id": article_id, "title": existing["title"], "path": existing["path"],
                                        "source_url": existing["source_url"], "post_id": ident, "cached": True})
                continue
            try:
                record = cache.store(article_id, item,
                                     build_epub(item, article_id, client, remote_images=not no_images,
                                                no_images=no_images))
            except (ReaderError, OSError) as exc:
                result["errors"].append({"post": ident, "stage": "epub", "error": str(exc)})
                continue
            stored.append(ident)
            result["created"] += 1
            detail = {"id": article_id, "title": record["title"], "path": record["path"],
                      "source_url": record["source_url"], "post_id": ident}
            if deliver is not None:
                try:
                    detail["delivery"] = deliver(Path(record["path"]))
                except ReaderError as exc:
                    result["errors"].append({"post": ident, "stage": "delivery", "error": str(exc)})
            result["items"].append(detail)
        if result["truncated"]:
            result["warnings"].append(f"X showed {len(eligible)} new posts but --limit is {limit}; "
                                      f"{limit} were handled and the rest wait for a later pass.")
        if result["undated"]:
            result["warnings"].append(f"{result['undated']} timeline entry/entries had no readable timestamp "
                                      "and were skipped rather than guessed into the window.")
        if not result["items"] and not result["errors"]:
            result["warnings"].append(f"No posts newer than {cutoff.isoformat(timespec='seconds')} from accounts "
                                      "you follow were new to this cache.")
        if not dry_run:
            updated = stored + [value for value in seen if value not in set(stored)]
            state["seen"] = updated[:SEEN_LIMIT]
            state["last_run"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
            state["last_counts"] = {key: result[key] for key in ("collected", "created", "cached", "skipped_seen",
                                                                "skipped_window", "skipped_ads", "truncated")}
            state["last_counts"]["errors"] = len(result["errors"])
            save_state(cache, state)
        return result
