"""Bounded local receipts for cloud items that are no longer in the active inbox.

The server lists active items only. A verified upload is recorded here so a consumed
item is never pushed again merely because its receipt disappeared from that listing.
Receipts are written only after the server confirms the item and its bytes; the file
contains no content, URLs, credentials, or source titles.
"""

from __future__ import annotations

import re
from pathlib import Path

from .cache import Cache
from .cloud import MAX_EPUB, json_bytes, parse_json
from .common import ReaderError, atomic_write, bounded_read, digest

VERSION = 1
MAX_RECEIPTS_BYTES = 1024 * 1024
ITEM_ID = re.compile(r"[0-9a-f-]{36}")
SHA256 = re.compile(r"[0-9a-f]{64}")


class Receipts:
    def __init__(self, path: Path, origin: str, items: dict[str, dict]):
        self.path = path
        self.origin = origin
        self.items = items
        self.changed = False

    @classmethod
    def load(cls, cache: Cache, origin: str) -> "Receipts":
        if not isinstance(cache, Cache):
            raise ReaderError("Cloud receipts need a cache directory")
        path = cache.root / f"cloud-receipts-{digest(origin)[:16]}.json"
        if not path.exists():
            return cls(path, origin, {})
        if path.is_symlink() or not path.is_file():
            raise ReaderError(f"Cloud receipt path is not a regular file: {path}")
        try:
            value = parse_json(bounded_read(path, MAX_RECEIPTS_BYTES))
        except (OSError, ReaderError, ValueError, TypeError, UnicodeError) as exc:
            raise ReaderError(f"Cloud receipt file is unreadable; move it aside to rebuild receipts: {path}: {exc}") from None
        if not isinstance(value, dict) or value.get("version") != VERSION or value.get("origin") != origin:
            raise ReaderError(f"Cloud receipt file is invalid or belongs to another origin; move it aside: {path}")
        raw = value.get("items")
        if not isinstance(raw, dict) or len(raw) > 4096:
            raise ReaderError(f"Cloud receipt file has an invalid item map; move it aside: {path}")
        items = {}
        for item_id, record in raw.items():
            if (not isinstance(item_id, str) or not ITEM_ID.fullmatch(item_id)
                    or not isinstance(record, dict)
                    or not isinstance(record.get("sha256"), str) or not SHA256.fullmatch(record["sha256"])
                    or type(record.get("size")) is not int or not 1 <= record["size"] <= MAX_EPUB):
                raise ReaderError(f"Cloud receipt file has an invalid entry; move it aside: {path}")
            items[item_id] = {"sha256": record["sha256"], "size": record["size"]}
        return cls(path, origin, items)

    def matches(self, item_id: str, metadata: dict) -> bool:
        record = self.items.get(item_id)
        return bool(record and record["sha256"] == metadata.get("sha256") and record["size"] == metadata.get("size"))

    def record(self, item_id: str, metadata: dict) -> None:
        value = {"sha256": metadata["sha256"], "size": metadata["size"]}
        if self.items.get(item_id) != value:
            self.items[item_id] = value
            self.changed = True

    def save(self) -> None:
        if not self.changed:
            return
        # Merge receipts another manual run may have added while this pass was in flight.
        try:
            if self.path.exists():
                current = parse_json(bounded_read(self.path, MAX_RECEIPTS_BYTES))
                raw = current.get("items") if isinstance(current, dict) else None
                if isinstance(raw, dict):
                    for item_id, record in raw.items():
                        if isinstance(record, dict) and isinstance(record.get("sha256"), str) \
                                and type(record.get("size")) is int:
                            self.items.setdefault(item_id, {"sha256": record["sha256"], "size": record["size"]})
        except (OSError, ReaderError, ValueError, TypeError, UnicodeError):
            pass
        payload = {"version": VERSION, "origin": self.origin, "items": self.items}
        atomic_write(self.path, json_bytes(payload) + b"\n", overwrite=True)
        self.changed = False
