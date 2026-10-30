from __future__ import annotations

import json
import os
import re
from contextlib import contextmanager
from pathlib import Path
from urllib.parse import urljoin
from xml.etree import ElementTree

from .cache import Cache
from .common import ReaderError, atomic_write, bounded_read, digest, http_url
from .epub import build_epub
from .extract import extract
from .model import source_id
from .network import PublicHTTP, decode_html
from .sources import load_sources


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


def subscriptions(cache: Cache) -> list[dict]:
    folder = cache.root / "subscriptions"
    if not folder.exists():
        return []
    result = []
    for path in sorted(folder.glob("*.json")):
        try:
            entry = json.loads(bounded_read(path, 8192))
            if not isinstance(entry, dict) or entry.get("name") != path.stem or not (entry.get("feed") or entry.get("export_dir")):
                raise ValueError("invalid subscription")
            result.append(entry)
        except ValueError as exc:
            raise ReaderError(f"Invalid publication configuration: {path}") from exc
    return result


def add(cache: Cache, name: str, feed: str, export_dir: str) -> dict:
    if not feed and not export_dir:
        raise ReaderError("Provide --feed URL, --export-dir DIRECTORY, or both")
    path = subscription_path(cache, name)
    if feed:
        feed = http_url(feed)
    if export_dir:
        folder = Path(export_dir).expanduser().resolve()
        if not folder.is_dir():
            raise ReaderError(f"Export directory does not exist: {folder}")
        export_dir = str(folder)
    record = {"name": name, "feed": feed, "export_dir": export_dir}
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


def sync(cache: Cache, client: PublicHTTP, *, name: str = "", limit: int = 10,
         dry_run: bool = False, no_images: bool = False) -> dict:
    configured = [s for s in subscriptions(cache) if not name or s["name"] == name]
    if not configured:
        raise ReaderError("No matching publications; add one with subscriptions add NAME --feed URL")
    result = {"items": [], "errors": [], "dry_run": dry_run}
    existing = cache.entries()
    by_source = {record.get("source_id"): record for record in existing}
    known_guids = {(r.get("provenance", {}).get("feed"), r.get("provenance", {}).get("feed_guid")) for r in existing}
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
        if not publication.get("feed"):
            continue
        feed = publication["feed"]
        try:
            entries = feed_entries(client.get(feed, 2 * 1024 * 1024).data, feed)
        except (ReaderError, OSError) as exc:
            result["errors"].append({"publication": label, "stage": "feed", "error": str(exc)})
            continue
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
    return result
