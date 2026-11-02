from __future__ import annotations

import json
import os
import re
from datetime import datetime, timezone
from pathlib import Path

from .common import ReaderError, atomic_write, bounded_read, digest, filename
from .extract import Article

ID_PATTERN = re.compile(r"[0-9a-f]{24}")


def default_cache() -> Path:
    return Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache"))) / "reader-send"


class Cache:
    def __init__(self, root: Path):
        self.root = root.expanduser().resolve()

    def key(self, identity: str, options: dict) -> str:
        return digest(json.dumps([1, identity, options], sort_keys=True, ensure_ascii=True))[:24]

    def directory(self, article_id: str) -> Path:
        if not ID_PATTERN.fullmatch(article_id):
            raise ReaderError("Cache ID must be the full 24-character ID shown by inspect")
        path = self.root / article_id
        if path.is_symlink():
            raise ReaderError(f"Cache entry is a symlink: {path}")
        return path

    def get(self, article_id: str, *, verify: bool = True) -> dict | None:
        directory = self.directory(article_id)
        record_path = directory / "article.json"
        if not record_path.exists():
            return None
        try:
            record = json.loads(bounded_read(record_path, 256 * 1024))
            if not isinstance(record, dict) or record.get("id") != article_id:
                raise ValueError("invalid record identity")
            name = record.get("filename", "")
            if not isinstance(name, str) or Path(name).name != name or not name.endswith(".epub"):
                raise ValueError("unsafe EPUB filename")
            epub = directory / name
            if epub.is_symlink() or not epub.is_file():
                raise ValueError("missing EPUB or symlink")
            if verify and digest(bounded_read(epub, 256 * 1024 * 1024)) != record.get("sha256"):
                raise ValueError("EPUB checksum mismatch")
            state = self.reading_state(record.get("source_id", ""))
            return {**record, **state, "path": str(epub)}
        except (ValueError, OSError, ReaderError) as exc:
            raise ReaderError(f"Corrupt cache entry {article_id}: {exc}; fetch with --refresh or convert again") from exc

    def store(self, article_id: str, article: Article, data: bytes, name: str | None = None) -> dict:
        directory = self.directory(article_id)
        name = name or filename(article.title, article_id)
        record = {"id": article_id, **article.metadata(), "filename": name, "size": len(data),
                  "sha256": digest(data), "cached_at": datetime.now(timezone.utc).isoformat()}
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        atomic_write(directory / name, data)
        atomic_write(directory / "article.json", (json.dumps(record, ensure_ascii=False, indent=2) + "\n").encode())
        return {**record, **self.reading_state(article.source_id), "path": str(directory / name)}

    def entries(self) -> list[dict]:
        if not self.root.exists():
            return []
        results = []
        for entry in sorted(self.root.iterdir()):
            if ID_PATTERN.fullmatch(entry.name):
                record = self.get(entry.name, verify=False)
                if record:
                    results.append(record)
        return results

    def reading_state(self, source_id: str) -> dict:
        if not source_id:
            return {}
        path = self.root / "state" / (digest(source_id) + ".json")
        if not path.exists():
            return {}
        try:
            state = json.loads(bounded_read(path, 4096))
            if (not isinstance(state, dict) or state.get("source_id") != source_id
                    or state.get("read_state") not in {"unread", "reading", "read"}
                    or not isinstance(state.get("progress"), (int, float)) or not 0 <= state["progress"] <= 1):
                raise ValueError("invalid reading state")
            return {key: state[key] for key in ("read_state", "progress", "state_updated_at")}
        except (ValueError, KeyError) as exc:
            raise ReaderError(f"Corrupt inbox reading state: {path}") from exc

    def mark(self, article_id: str, read_state: str, progress: float) -> dict:
        record = self.get(article_id, verify=False)
        if not record:
            raise ReaderError(f"No cached article with ID {article_id}")
        if read_state not in {"unread", "reading", "read"} or not 0 <= progress <= 1:
            raise ReaderError("Reading state must be unread/reading/read and progress between 0 and 1")
        identity = record.get("source_id") or "cache:" + article_id
        state = {"source_id": identity, "read_state": read_state, "progress": progress,
                 "state_updated_at": datetime.now(timezone.utc).isoformat()}
        atomic_write(self.root / "state" / (digest(identity) + ".json"), json.dumps(state).encode())
        return {**record, **state}
