"""Manual, explicit aggregation of the existing cloud inbox and annotation API.

This module adds no wire protocol and no state of its own: it lists the active inbox once, then
delegates every write to :class:`~reader_send.cloud.CloudClient` (``push``/``pull``) and to
the existing ``cloud_annotations`` helpers. It never deletes, never acknowledges, and never
runs in the background; the key is read by ``CloudClient`` from ``READER_SYNC_KEY`` only, so
a caller (Finder wrapper) can scope the environment without exposing the secret here.
A consumed item missing from that active listing is recognised by a durable local receipt:
the metadata POST returns the consumed item and no content PUT is sent.

Two directions, each bounded:

* push — every verified local cache entry that the cloud does not already hold. Entries
  downloaded from the cloud (``source_type == "cloud"`` / ``source_id == "cloud:<uuid>"``)
  are never pushed back, because their derived item ID would create a duplicate; they are
  reported under ``from_cloud``. Item IDs derived by ``prepare_push`` are stable, so
  re-running is idempotent; an existing ID with different bytes is reported as a conflict
  and never rewritten.
* pull — every ``ready`` cloud item whose SHA-256 is not cached yet, installed either as a
  normal cache entry (so ``card sync`` copies it) or as ``<uuid>.epub``/``<uuid>.json`` in an
  explicit incoming directory.

Each run opens with one inbox listing and is capped (``DEFAULT_MAX_ITEMS`` per direction)
because the API admits 60 authenticated requests per minute. A push costs two requests (POST
metadata, PUT content); a pull costs two as well, since ``CloudClient.pull`` re-lists the
inbox itself to confirm the item's state before downloading. A default run is therefore at
most 33 requests; leftovers are reported as ``deferred`` for the next run. ``dry_run`` only
lists: it performs local validation plus that one read-only listing and writes nothing, so
annotation exports are planned but not downloaded.
"""

from __future__ import annotations

import tempfile
from pathlib import Path

from .cache import Cache
from .cloud import MAX_EPUB, CloudClient, CloudContentReleased, json_bytes, prepare_push
from .cloud_annotations import (annotation_batches, download_annotations, load_annotations,
                                upload_annotations)
from .cloud_receipts import Receipts
from .common import ReaderError, atomic_write, bounded_read, digest
from .model import ReadingItem

DEFAULT_MAX_ITEMS = 8


def delivery_name(item: dict) -> str | None:
    """basename(source_path) when the firmware would accept it as the card file name, else None.

    Direct Wi-Fi sync names a book after its source_path (6–240 bytes, ``.epub``, no leading dot,
    control characters or ``\\:*?"<>|``); the card path uses the same name so both routes agree,
    e.g. ``X — Sun 2 Nov 2026.epub`` for a server digest.
    """
    path = item.get("source_path")
    if not isinstance(path, str):
        return None
    name = path.rsplit("/", 1)[-1]
    if (not 6 <= len(name.encode("utf-8")) <= 240 or not name.endswith(".epub") or name.startswith(".")
            or any(ord(char) < 32 or char in '\\:*?"<>|' for char in name)):
        return None
    return name


def cache_id_for(item_id: str) -> str:
    """Stable 24-character cache ID for a cloud item UUID, so no separate ledger is needed."""
    return digest(item_id)[:24]


def _cached_checksums(cache: Cache) -> dict[str, str]:
    return {record["id"]: record.get("sha256") for record in cache.entries()}


def _bounded(name: str, value) -> int:
    if type(value) is not int or not 1 <= value <= 64:
        raise ReaderError(f"{name} must be an integer from 1 to 64 (one manual run stays inside the 60/minute API budget)")
    return value


def push_cached(client: CloudClient, cache: Cache, remote: dict[str, dict], *,
                limit: int = DEFAULT_MAX_ITEMS, dry_run: bool = False,
                receipts: Receipts | None = None) -> dict:
    """Upload verified cache entries the cloud does not already hold. Idempotent by item ID.

    Entries that were downloaded from the cloud are never pushed back: their cache ID and
    source identity differ from the origin item, so a push would create a duplicate item.
    They are reported under ``from_cloud`` and do not consume the per-run limit. A verified
    receipt also suppresses a re-push after the server's active listing drops a consumed item.
    """
    receipts = receipts or Receipts.load(cache, client.origin)
    pushed, synced, conflicts, from_cloud = [], [], [], []
    deferred = 0
    try:
        for summary in cache.entries():
            if summary.get("source_type") == "cloud" or str(summary.get("source_id") or "").startswith("cloud:"):
                from_cloud.append(summary["id"])
                continue
            # prepare_push re-reads the EPUB, re-checks its SHA-256/size and the mimetype header.
            metadata, data = prepare_push(cache, summary["id"])
            existing = remote.get(metadata["id"])
            if existing is not None:
                if existing.get("sha256") != metadata["sha256"] or existing.get("size") != metadata["size"]:
                    conflicts.append({"cache_id": summary["id"], "item_id": metadata["id"],
                                      "reason": "Cloud item ID already holds different bytes; immutable IDs are never rewritten"})
                    continue
                if existing.get("state") != "uploading":
                    receipts.record(metadata["id"], metadata)
                    synced.append({"cache_id": summary["id"], "item_id": metadata["id"], "receipt": True})
                    continue
                # Same metadata but no stored content yet: the idempotent push below finishes it.
            elif receipts.matches(metadata["id"], metadata):
                synced.append({"cache_id": summary["id"], "item_id": metadata["id"], "receipt": True})
                continue
            if len(pushed) >= limit:
                deferred += 1
                continue
            if dry_run:
                pushed.append({"cache_id": summary["id"], "item_id": metadata["id"], "planned": True})
                continue
            client.push(metadata, data)
            receipts.record(metadata["id"], metadata)
            pushed.append({"cache_id": summary["id"], "item_id": metadata["id"]})
    finally:
        if not dry_run:
            receipts.save()
    return {"pushed": pushed, "already_synced": synced, "conflicts": conflicts,
            "deferred": deferred, "from_cloud": from_cloud}


def pull_ready(client: CloudClient, cache: Cache, items: list[dict], *,
               incoming_dir: Path | None = None, limit: int = DEFAULT_MAX_ITEMS,
               dry_run: bool = False, known: set[str] | None = None) -> dict:
    """Install ready cloud EPUBs that are not cached yet, verified by CloudClient.pull.

    Without ``incoming_dir`` each book becomes an ordinary cache entry, so the existing
    ``card sync`` copies it. With ``incoming_dir`` the verified ``<uuid>.epub``/``<uuid>.json``
    files stay there instead and the cache is not touched.
    A ready item another client acknowledged between the listing and its download is answered
    by the server's confirmed ``410 {"error": "content released"}``; it is reported under
    ``released`` (nothing is installed or acknowledged for it) and the pass continues. Every
    other failure, including any unconfirmed 410 or a 404, still aborts the pass.
    """
    incoming = incoming_dir.expanduser() if incoming_dir is not None else None
    known = set(known) if known is not None else set(value for value in _cached_checksums(cache).values() if value)
    pulled, already_cached, released = [], [], []
    deferred = 0
    for item in items:
        if item["state"] != "ready":
            continue
        if item["sha256"] in known:
            already_cached.append(item["id"])
            continue
        cache_id = cache_id_for(item["id"])
        if cache.get(cache_id, verify=False) is not None:
            raise ReaderError(f"Local cache ID {cache_id} already exists for another article; move it aside before pulling {item['id']}")
        if len(pulled) >= limit:
            deferred += 1
            continue
        if dry_run:
            pulled.append({"item_id": item["id"], "cache_id": cache_id, "planned": True})
            continue
        if incoming is not None:
            destination = incoming / (item["id"] + ".epub")
            if destination.exists() or destination.is_symlink():
                if destination.is_symlink() or not destination.is_file():
                    raise ReaderError(f"Incoming path is not a regular file: {destination}")
                if digest(bounded_read(destination, MAX_EPUB)) != item["sha256"]:
                    raise ReaderError(f"Incoming file differs from the cloud copy; it was not overwritten: {destination}")
                already_cached.append(item["id"])
                continue
            try:
                result = client.pull(item["id"], incoming, item=item)
            except CloudContentReleased:
                released.append(item["id"])
                continue
            known.add(item["sha256"])
            pulled.append({"item_id": item["id"], "path": result["output"], "metadata": result["metadata"]})
            continue
        # A fresh staging directory: CloudClient.pull verifies SHA-256, size and mimetype header
        # and writes nothing else; only then is the book adopted as a cache entry.
        with tempfile.TemporaryDirectory(prefix="reader-send-cloud-") as staging:
            try:
                result = client.pull(item["id"], Path(staging), item=item)
            except CloudContentReleased:
                released.append(item["id"])
                continue
            data = Path(result["output"]).read_bytes()
        if len(data) != item["size"] or digest(data) != item["sha256"]:
            raise ReaderError(f"Downloaded EPUB for {item['id']} changed before it was stored; nothing written")
        article = ReadingItem(title=item["title"], author="", source_url=item.get("original_url") or "",
                              date="", language="", content="", images=[],
                              source_type="cloud", source_id="cloud:" + item["id"])
        record = cache.store(cache_id, article, data, delivery_name(item))
        known.add(item["sha256"])
        pulled.append({"item_id": item["id"], "cache_id": cache_id, "path": record["path"]})
    return {"pulled": pulled, "already_cached": already_cached, "released": released, "deferred": deferred}


def return_annotations(client: CloudClient, source: Path, *, device_id: str | None = None,
                       item_id: str | None = None, dry_run: bool = False) -> dict:
    """Upload card/native or API-format annotations with the existing batching rules."""
    batches = annotation_batches(load_annotations(source), device_id=device_id, item_id=item_id)
    if dry_run:
        return {"dry_run": True, "batches": len(batches),
                "body_bytes": [len(body) for body in batches]}
    return upload_annotations(client, batches)


def export_annotations(client: CloudClient, output: Path, *, after: int = 0, limit: int = 100,
                       dry_run: bool = False) -> dict:
    """Write every server annotation page to one private file (NDJSON for .ndjson/.jsonl)."""
    output = output.expanduser()
    export = output.suffix.lower() in {".ndjson", ".jsonl"}
    if dry_run:
        # No request and no invented counts: dry-run only reports the planned managed file.
        if output.exists() and (output.is_symlink() or not output.is_file()):
            raise ReaderError(f"Annotation output path is not a regular file: {output}")
        return {"output": str(output), "planned": True, "written": False,
                "format": "ndjson" if export else "json"}
    result = download_annotations(client, after=after, limit=limit, export=export)
    if export:
        data = result.pop("ndjson")
    else:
        data = json_bytes(result) + b"\n"
    atomic_write(output, data, overwrite=True)
    return {"output": str(output), "count": len(result["annotations"]), "after": result["after"],
            "written": True}


def sync(client: CloudClient, cache: Cache, *, incoming_dir: Path | None = None,
         annotations_in: Path | None = None, annotations_out: Path | None = None,
         device_id: str | None = None, item_id: str | None = None,
         push: bool = True, pull: bool = True,
         max_push: int = DEFAULT_MAX_ITEMS, max_pull: int = DEFAULT_MAX_ITEMS,
         dry_run: bool = False) -> dict:
    """One explicit, bounded pass over the private inbox; returns a JSON-serialisable summary.

    ``dry_run`` validates locally, performs the single read-only inbox listing and writes
    nothing (no push, no download, no annotation upload/export).
    """
    if not isinstance(client, CloudClient):
        raise ReaderError("Cloud sync needs a CloudClient built from the READER_SYNC_KEY environment variable")
    if not isinstance(cache, Cache):
        raise ReaderError("Cloud sync needs a cache directory")
    if not push and not pull and annotations_in is None and annotations_out is None:
        raise ReaderError("Cloud sync has nothing selected; enable push/pull or pass annotations")
    _bounded("max_push", max_push)
    _bounded("max_pull", max_pull)

    items = client.items()
    remote = {item["id"]: item for item in items}
    receipts = Receipts.load(cache, client.origin) if push else None
    result = {
        "origin": client.origin,
        "dry_run": dry_run,
        "remote": {
            "total": len(items),
            "ready": sum(item["state"] == "ready" for item in items),
            "uploading": sum(item["state"] == "uploading" for item in items),
            "consumed": sum(item["state"] == "consumed" for item in items),
            "receipts": len(receipts.items) if receipts is not None else 0,
        },
    }
    if push:
        result["push"] = push_cached(client, cache, remote, limit=max_push, dry_run=dry_run, receipts=receipts)
    if pull:
        result["pull"] = pull_ready(client, cache, items, incoming_dir=incoming_dir,
                                    limit=max_pull, dry_run=dry_run)
    if annotations_in is not None:
        result["annotations_in"] = return_annotations(client, annotations_in, device_id=device_id,
                                                      item_id=item_id, dry_run=dry_run)
    if annotations_out is not None:
        result["annotations_out"] = export_annotations(client, annotations_out, dry_run=dry_run)
    return result
