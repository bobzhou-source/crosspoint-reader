"""Card-side Inbox maintenance: repack single X posts into digests, and auto-delete read feed items.

Both commands plan first and change nothing unless applied. Nothing is ever unlinked except trash
entries older than ``TRASH_DAYS`` and the per-book reader cache of a trashed book:

* repack writes new digest EPUBs and *moves* the singles they contain to ``/books/Inbox/.singles-archive/``;
* cleanup *moves* finished/expired feed items to ``/books/Inbox/.trash/``, clears their
  ``/.crosspoint/epub_<std::hash(path)>`` cache and their ``recent.json`` entry, and purges trash
  entries after seven days. Native annotation records under ``/.crosspoint/annotations/`` are never
  touched, and a book whose annotations the server has not confirmed is held.
"""
from __future__ import annotations

import io
import json
import os
import shutil
import tempfile
import zipfile
from datetime import datetime, timedelta, timezone
from pathlib import Path
from xml.etree import ElementTree

from . import feed_digest
from .cloud_annotations import map_annotation
from .common import ReaderError, digest

INBOX = Path("books/Inbox")
ARCHIVE_NAME = ".singles-archive"
TRASH_NAME = ".trash"
CROSSPOINT = Path(".crosspoint")
RECENTS = CROSSPOINT / "recent.json"
LIBRARY_DIRTY = CROSSPOINT / "library.dirty"
SYNC_LEDGER = CROSSPOINT / "reader-sync/items"
SYNC_NOTES = CROSSPOINT / "reader-sync/notes"
SYNC_CONFIG = CROSSPOINT / "reader-sync.json"
ANNOTATIONS = CROSSPOINT / "annotations"
X_FINISHED_DAYS = 3
X_UNREAD_DAYS = 30
ARTICLE_FINISHED_DAYS = 14
TRASH_DAYS = 7
PLAUSIBLE = datetime(2025, 1, 1, tzinfo=timezone.utc)
STATE_VERSION = 1


def std_hash(text: str) -> int:
    """libstdc++ std::hash<std::string> on the 32-bit ESP32-S3 (murmur2, seed 0xc70f6907)."""
    data = text.encode("utf-8")
    mask, mul = 0xFFFFFFFF, 0x5BD1E995
    value, index = (0xC70F6907 ^ len(data)) & mask, 0
    while len(data) - index >= 4:
        k = (int.from_bytes(data[index:index + 4], "little") * mul) & mask
        k ^= k >> 24
        value = ((value * mul) & mask) ^ ((k * mul) & mask)
        index += 4
    rest = len(data) - index
    if rest == 3:
        value ^= data[index + 2] << 16
    if rest >= 2:
        value ^= data[index + 1] << 8
    if rest >= 1:
        value ^= data[index]
        value = (value * mul) & mask
    value ^= value >> 13
    value = (value * mul) & mask
    return value ^ (value >> 15)


def annotation_key(card_path: str) -> str:
    """AnnotationStore::sourceKey: 64-bit FNV-1a of the book path, 16 lowercase hex digits."""
    value = 14695981039346656037
    for byte in card_path.encode("utf-8"):
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def card_path(root: Path, path: Path) -> str:
    return "/" + path.relative_to(root).as_posix()


def cache_dir(root: Path, book: str) -> Path:
    return root / CROSSPOINT / f"epub_{std_hash(book)}"


def _json(path: Path, limit: int = 256 * 1024):
    try:
        if path.is_symlink() or not path.is_file() or path.stat().st_size > limit:
            return None
        return json.loads(path.read_bytes())
    except (OSError, ValueError):
        return None


def inbox_books(root: Path) -> list[Path]:
    inbox = root / INBOX
    if not inbox.is_dir():
        raise ReaderError(f"No /books/Inbox on {root}")
    return sorted(path for path in inbox.iterdir()
                  if path.suffix.lower() == ".epub" and not path.name.startswith(".") and path.is_file()
                  and not path.is_symlink())


def synced_items(root: Path) -> dict[str, str]:
    """Card path -> server item id from the firmware's direct-sync ledger."""
    folder = root / SYNC_LEDGER
    found: dict[str, str] = {}
    if folder.is_dir():
        for entry in sorted(folder.glob("*.json")):
            value = _json(entry, 16 * 1024)
            if isinstance(value, dict) and isinstance(value.get("path"), str) and isinstance(value.get("id"), str):
                found[value["path"]] = value["id"]
    return found


def native_annotations(root: Path, book: str) -> list[dict]:
    folder = root / ANNOTATIONS / annotation_key(book)
    if not folder.is_dir():
        return []
    records = []
    for entry in sorted(folder.glob("*.json")):
        value = _json(entry, 64 * 1024)
        if isinstance(value, dict) and value.get("sourcePath") == book:
            records.append(value)
        elif not entry.name.startswith("._"):
            records.append({"unreadable": entry.name})  # unknown content holds the book
    return records


def device_id(root: Path) -> str | None:
    """The firmware's nonsecret device id (the device key in the same file is never read out)."""
    value = _json(root / SYNC_CONFIG, 64 * 1024)
    ident = value.get("device_id") if isinstance(value, dict) else None
    return ident if isinstance(ident, str) and ident else None


def annotations_confirmed(root: Path, records: list[dict], device: str | None, confirmed: set[str]) -> bool:
    """True when every native record has a server receipt (firmware notes/<id>.receipt or ``confirmed``)."""
    if not records:
        return True
    if device is None:
        return False
    for record in records:
        if "unreadable" in record:
            return False
        try:
            ident = map_annotation(record, device_id=device)["id"]
        except ReaderError:
            return False
        receipt = root / SYNC_NOTES / f"{ident}.receipt"
        if ident not in confirmed and not (receipt.is_file() and receipt.stat().st_size == 64):
            return False
    return True


def read_progress(root: Path, book: str) -> tuple[int, int, int] | None:
    """progress.bin: u16 spine, u16 page, [u16 chapter page count], [u32 text offset], little endian."""
    path = cache_dir(root, book) / "progress.bin"
    try:
        data = path.read_bytes() if path.is_file() and not path.is_symlink() else b""
    except OSError:
        return None
    if len(data) not in (4, 6, 10):
        return None
    total = int.from_bytes(data[4:6], "little") if len(data) >= 6 else 0
    return int.from_bytes(data[0:2], "little"), int.from_bytes(data[2:4], "little"), total


def is_finished(progress: tuple[int, int, int] | None, spine_items: int) -> bool:
    """At the end: the end-of-book spine sentinel, or the last page of the last spine item."""
    if progress is None or spine_items <= 0:
        return False
    spine, page, total = progress
    if spine >= spine_items:
        return True
    return spine == spine_items - 1 and total > 0 and page != 0xFFFF and page + 1 >= total


def _mtime(path: Path, now: datetime) -> datetime | None:
    try:
        moment = datetime.fromtimestamp(path.stat().st_mtime, timezone.utc)
    except OSError:
        return None
    return moment if PLAUSIBLE <= moment <= now + timedelta(days=1) else None


def _stamp(value) -> datetime | None:
    try:
        return datetime.fromisoformat(value) if isinstance(value, str) else None
    except ValueError:
        return None


def _classify(data: bytes) -> tuple[str, int]:
    """("x-digest" | "x-post" | "article", spine item count)."""
    spine = 0
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            container = ElementTree.fromstring(archive.read("META-INF/container.xml"))
            rootfile = container.find(".//c:rootfile", feed_digest.NS)
            opf = ElementTree.fromstring(archive.read(rootfile.get("full-path")))
            spine = len(opf.findall(".//opf:spine/opf:itemref", feed_digest.NS))
    except (zipfile.BadZipFile, KeyError, ElementTree.ParseError, AttributeError, TypeError):
        return "unreadable", 0
    if feed_digest.read_manifest(data) is not None:
        return "x-digest", spine
    try:
        feed_digest.read_single(data)
        return "x-post", spine
    except ReaderError:
        return "article", spine


def load_state(path: Path) -> dict:
    value = _json(path, 4 * 1024 * 1024)
    if not isinstance(value, dict) or value.get("version") != STATE_VERSION:
        return {"version": STATE_VERSION, "seen": {}, "finished": {}, "trashed": {}}
    for key in ("seen", "finished", "trashed"):
        if not isinstance(value.get(key), dict):
            value[key] = {}
    return value


def save_state(path: Path, state: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".inbox-state-", dir=path.parent)
    with os.fdopen(fd, "w", encoding="utf-8") as stream:
        json.dump(state, stream, ensure_ascii=False, indent=1, sort_keys=True)
    os.replace(temporary, path)


def plan_cleanup(root: Path, state: dict, now: datetime, *, confirmed: set[str] | None = None,
                 delivered: set[str] | None = None) -> dict:
    """Plan the auto-delete pass. ``state`` is updated in place with first-seen/finished times.

    ``delivered`` adds card paths known to be server deliveries (the Mac card ledger) to those in the
    firmware ledger; anything else that is not a digest or an X post is never a cleanup candidate.
    """
    synced = synced_items(root)
    known = set(synced) | set(delivered or ())
    device = device_id(root)
    files, purge = [], []
    for path in inbox_books(root):
        book = card_path(root, path)
        try:
            data = path.read_bytes()
        except OSError:
            continue
        kind, spine = _classify(data)
        seen = _stamp(state["seen"].get(book))
        if seen is None:
            state["seen"][book] = now.isoformat()
            seen = now
        delivered_at = _mtime(path, now) or seen
        progress = read_progress(root, book)
        finished = is_finished(progress, spine)
        finished_at = None
        if finished:
            first = _stamp(state["finished"].get(book))
            if first is None:
                state["finished"][book] = now.isoformat()
                first = now
            finished_at = first
        else:
            state["finished"].pop(book, None)
        entry = {"file": book, "kind": kind, "finished": finished, "delivered_at": delivered_at.isoformat(),
                 "finished_at": finished_at.isoformat() if finished_at else None, "action": "keep"}
        if kind in ("unreadable",) or (kind == "article" and book not in known):
            entry["reason"] = "not a synced feed item"
        elif kind in ("x-digest", "x-post"):
            if finished_at and now - finished_at >= timedelta(days=X_FINISHED_DAYS):
                entry.update(action="trash", reason=f"finished {X_FINISHED_DAYS}+ days ago")
            elif now - delivered_at >= timedelta(days=X_UNREAD_DAYS):
                entry.update(action="trash", reason=f"delivered {X_UNREAD_DAYS}+ days ago")
            else:
                entry["reason"] = "recent"
        elif finished_at and now - finished_at >= timedelta(days=ARTICLE_FINISHED_DAYS):
            entry.update(action="trash", reason=f"finished {ARTICLE_FINISHED_DAYS}+ days ago")
        else:
            entry["reason"] = "unfinished long post" if not finished else "finished recently"
        if entry["action"] == "trash":
            notes = native_annotations(root, book)
            if not annotations_confirmed(root, notes, device, confirmed or set()):
                entry.update(action="hold", reason="annotations not yet confirmed by the server")
            entry["annotations"] = len(notes)
            entry["cache"] = card_path(root, cache_dir(root, book))
        files.append(entry)
    trash = root / INBOX / TRASH_NAME
    if trash.is_dir():
        for path in sorted(trash.iterdir()):
            if path.name.startswith("._") or not path.is_file():
                continue
            stamp = _stamp(state["trashed"].get(path.name))
            if stamp is None:
                state["trashed"][path.name] = now.isoformat()
                stamp = now
            if now - stamp >= timedelta(days=TRASH_DAYS):
                purge.append(card_path(root, path))
    counts = {action: sum(1 for entry in files if entry["action"] == action) for action in ("keep", "trash", "hold")}
    return {"files": files, "purge": purge, "counts": {**counts, "purge": len(purge)}}


def _free_name(folder: Path, name: str) -> Path:
    candidate = folder / name
    stem, suffix = Path(name).stem, Path(name).suffix
    counter = 2
    while candidate.exists() or candidate.is_symlink():
        candidate = folder / f"{stem}-{counter}{suffix}"
        counter += 1
    return candidate


def remove_recents(root: Path, books: set[str]) -> int:
    path = root / RECENTS
    value = _json(path, 1024 * 1024)
    if not books or not isinstance(value, dict) or not isinstance(value.get("books"), list):
        return 0
    kept = [book for book in value["books"] if not (isinstance(book, dict) and book.get("path") in books)]
    removed = len(value["books"]) - len(kept)
    if removed:
        value["books"] = kept
        fd, temporary = tempfile.mkstemp(prefix=".recent-", dir=path.parent)
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(value, stream, ensure_ascii=False)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    return removed


def _mark_library_dirty(root: Path) -> None:
    marker = root / LIBRARY_DIRTY
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_bytes(b"1")


def apply_cleanup(root: Path, plan: dict, state: dict, now: datetime) -> dict:
    """Move planned files to the trash, clear their caches and recents, purge week-old trash."""
    trash = root / INBOX / TRASH_NAME
    moved, caches = [], 0
    for entry in plan["files"]:
        if entry["action"] != "trash":
            continue
        source = root / entry["file"].lstrip("/")
        if not source.is_file():
            continue
        trash.mkdir(exist_ok=True)
        target = _free_name(trash, source.name)
        os.rename(source, target)
        state["trashed"][target.name] = now.isoformat()
        state["seen"].pop(entry["file"], None)
        state["finished"].pop(entry["file"], None)
        moved.append(entry["file"])
        cache = cache_dir(root, entry["file"])
        if cache.is_dir() and not cache.is_symlink():
            shutil.rmtree(cache)
            caches += 1
    purged = 0
    for book in plan["purge"]:
        path = root / book.lstrip("/")
        if path.parent == trash and path.is_file() and not path.is_symlink():
            path.unlink()
            state["trashed"].pop(path.name, None)
            purged += 1
    recents = remove_recents(root, set(moved))
    if moved:
        _mark_library_dirty(root)
    return {"trashed": len(moved), "caches_cleared": caches, "recents_removed": recents, "purged": purged}


def state_path(cache_root: Path, volume_uuid: str) -> Path:
    """Mac-side cleanup state (first-seen, first-finished and trashed times) for one card."""
    return cache_root / f".inbox-cleanup-{volume_uuid}.json"


def run_cleanup(root: Path, state_file: Path, *, apply: bool = False, now: datetime | None = None,
                delivered: set[str] | None = None, confirmed: set[str] | None = None) -> dict:
    """Plan (and with ``apply`` perform) one cleanup pass; the timing state is saved either way."""
    now = now or datetime.now(timezone.utc)
    state = load_state(state_file)
    plan = plan_cleanup(root, state, now, delivered=delivered, confirmed=confirmed)
    applied = apply_cleanup(root, plan, state, now) if apply else None
    save_state(state_file, state)
    return {"dry_run": not apply, "counts": plan["counts"], "files": plan["files"], "purge": plan["purge"],
            "applied": applied}



def plan_repack(root: Path, tz_name: str = feed_digest.DEFAULT_TZ, *, max_posts: int = feed_digest.MAX_POSTS,
                max_bytes: int = feed_digest.MAX_BYTES) -> dict:
    """Plan digests for the single X posts in /books/Inbox; nothing is written."""
    synced = synced_items(root)
    inbox = root / INBOX
    posts, sources, kept = [], {}, []
    existing_days: dict = {}
    existing_keys: set[str] = set()
    taken = set()
    for path in inbox_books(root):
        book = card_path(root, path)
        taken.add(path.name)
        try:
            data = path.read_bytes()
        except OSError as exc:
            kept.append({"file": book, "reason": f"unreadable: {exc}"})
            continue
        found = feed_digest.read_manifest(data)
        if found is not None:
            day = datetime.fromisoformat(found["day"]).date()
            existing_days[day] = max(existing_days.get(day, 0), int(found["volume"]))
            existing_keys.update(item["key"] for item in found["items"])
            continue
        try:
            post = feed_digest.read_single(data, synced.get(book))
        except ReaderError:
            continue  # articles and other books are not touched
        if native_annotations(root, book):
            kept.append({"file": book, "reason": "has annotations; left as a single"})
            continue
        posts.append(post)
        sources.setdefault(post.key, []).append(book)
    fresh = [post for post in posts if post.key not in existing_keys]
    volumes = feed_digest.plan_volumes(fresh, tz_name, existing=existing_days, max_posts=max_posts,
                                       max_bytes=max_bytes)
    digests, archive = [], []
    for volume in volumes:
        name = volume.filename
        number = volume.number
        while name in taken:  # an unrelated file already has this name: take the next volume number
            number += 1
            volume = feed_digest.Volume(volume.source, volume.day, number, volume.posts)
            name = volume.filename
        taken.add(name)
        files = [book for post in volume.posts for book in sources[post.key]]
        digests.append({"file": card_path(root, inbox / name), "volume": volume, "posts": len(volume.posts),
                        "singles": files})
        archive.extend(files)
    duplicates = [book for post in posts if post.key in existing_keys for book in sources.pop(post.key, [])]
    archive.extend(duplicates)
    return {"singles": len(archive), "digests": digests, "archive": sorted(set(archive)),
            "already_in_digest": sorted(set(duplicates)), "kept": kept, "timezone": tz_name}


def describe_repack(plan: dict) -> dict:
    return {"singles": plan["singles"], "digest_count": len(plan["digests"]),
            "digests": [{"file": entry["file"], "posts": entry["posts"], "singles": entry["singles"]}
                        for entry in plan["digests"]],
            "archive_to": f"/{INBOX.as_posix()}/{ARCHIVE_NAME}/",
            "already_in_digest": plan["already_in_digest"], "kept": plan["kept"], "timezone": plan["timezone"]}


def apply_repack(root: Path, plan: dict) -> dict:
    """Write each digest (verified by read-back), then move its singles to the archive folder."""
    inbox = root / INBOX
    archive = inbox / ARCHIVE_NAME
    written, moved = [], []
    for entry in plan["digests"]:
        target = root / entry["file"].lstrip("/")
        data = feed_digest.build_digest(entry["volume"], plan["timezone"], generator="companion-repack")
        if target.exists():
            if digest(target.read_bytes()) != digest(data):
                raise ReaderError(f"{entry['file']} appeared with different bytes; nothing overwritten")
        else:
            fd, temporary = tempfile.mkstemp(prefix=".repack-", suffix=".tmp", dir=inbox)
            with os.fdopen(fd, "wb") as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            os.rename(temporary, target)
            if digest(target.read_bytes()) != digest(data):
                raise ReaderError(f"Read-back of {entry['file']} failed; its singles were not moved")
            written.append(entry["file"])
        for book in entry["singles"]:
            source = root / book.lstrip("/")
            if source.is_file():
                archive.mkdir(exist_ok=True)
                os.rename(source, _free_name(archive, source.name))
                moved.append(book)
    for book in plan["already_in_digest"]:
        source = root / book.lstrip("/")
        if source.is_file():
            archive.mkdir(exist_ok=True)
            os.rename(source, _free_name(archive, source.name))
            moved.append(book)
    recents = remove_recents(root, set(moved))
    if written or moved:
        _mark_library_dirty(root)
    return {"digests_written": len(written), "singles_archived": len(moved), "recents_removed": recents}
