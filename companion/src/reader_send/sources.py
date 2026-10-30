from __future__ import annotations

import csv
import io
import stat
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from urllib.parse import unquote, urlsplit

from .common import MAX_HTML_BYTES, MAX_IMAGE_BYTES, ReaderError, bounded_read, digest


@dataclass
class LocalSource:
    name: str
    data: bytes
    identity: str
    metadata: dict[str, str]
    path: Path | None = None
    root: Path | None = None
    archive: Path | None = None
    member: str = ""

    def image(self, reference: str) -> bytes:
        parts = urlsplit(reference)
        if parts.scheme or parts.netloc or "\\" in reference:
            raise ReaderError("Only relative local image paths inside the input folder/export are allowed")
        name = unquote(parts.path)
        if not name or name.startswith("/") or "\x00" in name:
            raise ReaderError("Unsafe local image path")
        if self.archive:
            target = PurePosixPath(self.member).parent / name
            # Normalize without extracting or allowing an escape from archive root.
            segments: list[str] = []
            for segment in target.parts:
                if segment == "..":
                    if not segments:
                        raise ReaderError("Image escapes the export archive")
                    segments.pop()
                elif segment not in ("", "."):
                    segments.append(segment)
            with zipfile.ZipFile(self.archive) as archive:
                try:
                    info = archive.getinfo("/".join(segments))
                except KeyError as exc:
                    raise ReaderError(f"Image is not in export: {name}") from exc
                return read_member(archive, info, MAX_IMAGE_BYTES)
        assert self.path is not None and self.root is not None
        target = (self.path.parent / name).resolve()
        if not target.is_relative_to(self.root):
            raise ReaderError("Image escapes the local input folder")
        return bounded_read(target, MAX_IMAGE_BYTES)


def read_member(archive: zipfile.ZipFile, info: zipfile.ZipInfo, limit: int) -> bytes:
    if info.file_size > limit or stat.S_ISLNK(info.external_attr >> 16) or info.flag_bits & 1:
        raise ReaderError(f"Oversized, encrypted, or symlink export member: {info.filename}")
    with archive.open(info) as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ReaderError(f"Export member exceeds {limit // 1024} KiB: {info.filename}")
    return data


def csv_metadata(data: bytes) -> dict[str, dict[str, str]]:
    result = {}
    for row in csv.DictReader(io.StringIO(data.decode("utf-8-sig", "replace"))):
        metadata = {
            "title": row.get("title", ""),
            "author": row.get("author", "") or row.get("author_name", ""),
            "date": row.get("post_date", "") or row.get("published_at", ""),
            "source_url": row.get("post_url", "") or row.get("url", ""),
        }
        for key in (row.get("post_id"), row.get("id"), row.get("slug")):
            if key:
                result[key] = metadata
    return result


def load_sources(path: Path):
    path = path.expanduser().resolve()
    if not path.exists():
        raise ReaderError(f"Input does not exist: {path}")
    if path.is_file() and path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as archive:
            infos = archive.infolist()
            if len(infos) > 10000 or sum(i.file_size for i in infos) > 256 * 1024 * 1024:
                raise ReaderError("Export exceeds 10,000 entries / 256 MiB uncompressed")
            names = [i.filename for i in infos]
            if len(names) != len(set(names)):
                raise ReaderError("Export has ambiguous duplicate member names")
            for name in names:
                pure = PurePosixPath(name)
                if pure.is_absolute() or ".." in pure.parts or "\\" in name:
                    raise ReaderError(f"Unsafe export member path: {name}")
            metadata = {}
            for info in infos:
                if PurePosixPath(info.filename).name == "posts.csv":
                    metadata.update(csv_metadata(read_member(archive, info, MAX_HTML_BYTES)))
            html_files = sorted((i for i in infos if PurePosixPath(i.filename).suffix.lower() in {".html", ".htm"}), key=lambda i: i.filename)
            if not html_files:
                raise ReaderError("Export contains no HTML posts")
            for info in html_files:
                stem = PurePosixPath(info.filename).stem
                data = read_member(archive, info, MAX_HTML_BYTES)
                yield LocalSource(info.filename, data, f"{path.as_uri()}#{info.filename}:{digest(data)}",
                                  metadata.get(stem, metadata.get(stem.split(".")[0], {})), archive=path, member=info.filename)
        return
    if path.is_dir():
        root = path
        html_files = sorted(p for p in path.rglob("*") if p.suffix.lower() in {".html", ".htm"} and p.is_file())
        metadata = {}
        for csv_path in sorted(path.rglob("posts.csv")):
            if csv_path.resolve().is_relative_to(root):
                metadata.update(csv_metadata(bounded_read(csv_path, MAX_HTML_BYTES)))
    elif path.suffix.lower() in {".html", ".htm"}:
        root = path.parent
        html_files = [path]
        metadata = {}
    else:
        raise ReaderError("Input must be an HTML file, directory of HTML posts, or Substack ZIP export")
    if not html_files or len(html_files) > 1000:
        raise ReaderError("Input must contain between 1 and 1,000 HTML posts")
    for item in html_files:
        resolved = item.resolve()
        if not resolved.is_relative_to(root):
            raise ReaderError(f"HTML symlink escapes input folder: {item}")
        data = bounded_read(resolved, MAX_HTML_BYTES)
        stem = item.stem
        yield LocalSource(str(item.relative_to(root)), data, f"{resolved.as_uri()}:{digest(data)}",
                          metadata.get(stem, metadata.get(stem.split(".")[0], {})), path=resolved, root=root)
