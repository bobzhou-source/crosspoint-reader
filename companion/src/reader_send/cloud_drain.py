"""Repeat bounded cloud inbox batches without manual retries.

The normal ``cloud sync`` command deliberately stays one small pass.  This helper is
only used by a launcher when the user explicitly asks to finish a large backlog: each
pass keeps the existing request budget, annotations are uploaded before the first pass
and downloaded once after the last, and a fixed gap keeps successive passes below the
server's 60-requests-per-minute limit.
"""

from __future__ import annotations

import time
from pathlib import Path
from typing import Callable

from .cache import Cache
from .cloud import CloudClient
from .cloud_sync import (DEFAULT_MAX_ITEMS, export_annotations, return_annotations,
                         sync)
from .common import ReaderError

DRAIN_WAIT_SECONDS = 60


def drain(client: CloudClient, cache: Cache, *, incoming_dir: Path | None = None,
          annotations_in: Path | None = None, annotations_out: Path | None = None,
          device_id: str | None = None, item_id: str | None = None,
          push: bool = True, pull: bool = True,
          max_push: int = DEFAULT_MAX_ITEMS, max_pull: int = DEFAULT_MAX_ITEMS,
          dry_run: bool = False, wait: Callable[[float], None] = time.sleep,
          progress: Callable[[str], None] | None = None) -> dict:
    """Run bounded passes until deferred work is empty or no further progress is made.

    Network and validation errors propagate immediately; no failed batch is retried.
    A batch that reports conflicts, or leaves both deferred buckets unchanged without
    moving a new item, stops the loop and returns ``complete: false`` with the
    remaining counts.  Annotation input is sent once before the loop, and annotation
    output is fetched once after it (including after a failure).
    """
    if not isinstance(client, CloudClient):
        raise ReaderError("Cloud drain needs a CloudClient built from the READER_SYNC_KEY environment variable")
    if not isinstance(cache, Cache):
        raise ReaderError("Cloud drain needs a cache directory")
    if not push and not pull and annotations_in is None and annotations_out is None:
        raise ReaderError("Cloud sync has nothing selected; enable push/pull or pass annotations")
    for name, value in (("max_push", max_push), ("max_pull", max_pull)):
        if type(value) is not int or not 1 <= value <= 64:
            raise ReaderError(f"{name} must be an integer from 1 to 64")
    if dry_run:
        return sync(client, cache, incoming_dir=incoming_dir, annotations_in=annotations_in,
                    annotations_out=annotations_out, device_id=device_id, item_id=item_id,
                    push=push, pull=pull, max_push=max_push, max_pull=max_pull, dry_run=True)

    if annotations_out is not None:
        # Validate the destination before any annotation can be accepted.
        export_annotations(client, annotations_out, dry_run=True)

    annotations_uploaded = None
    annotations_exported = None
    annotations_error = None
    total_pushed = 0
    total_pulled = 0
    total_synced = 0
    total_cached = 0
    total_released = 0
    total_from_cloud = 0
    conflicts = []
    deferred = 0
    previous_deferred = None
    seen_work = set()
    stopped = None
    batches = 0
    remote = None

    try:
        if annotations_in is not None:
            annotations_uploaded = return_annotations(
                client, annotations_in, device_id=device_id, item_id=item_id)
        while push or pull:
            result = sync(
                client, cache, incoming_dir=incoming_dir,
                push=push, pull=pull, max_push=max_push, max_pull=max_pull,
            )
            batches += 1
            remote = result.get("remote", remote)
            pushed = result.get("push", {})
            pulled = result.get("pull", {})
            new_work = False
            for prefix, items in (("p", pushed.get("pushed", ())), ("l", pulled.get("pulled", ()))):
                for item in items:
                    identity = (prefix, item.get("item_id") or item.get("cache_id") or item.get("path"))
                    if identity not in seen_work:
                        seen_work.add(identity)
                        new_work = True
            total_pushed += len(pushed.get("pushed", ()))
            total_synced += len(pushed.get("already_synced", ()))
            total_from_cloud += len(pushed.get("from_cloud", ()))
            total_pulled += len(pulled.get("pulled", ()))
            total_cached += len(pulled.get("already_cached", ()))
            total_released += len(pulled.get("released", ()))
            push_deferred = pushed.get("deferred", 0)
            pull_deferred = pulled.get("deferred", 0)
            deferred = push_deferred + pull_deferred
            if pushed.get("conflicts"):
                conflicts = pushed["conflicts"]
                stopped = "cloud item ID conflict"
                break
            if progress is not None:
                progress(f"cloud batch {batches}: pushed {len(pushed.get('pushed', ()))}, "
                         f"pulled {len(pulled.get('pulled', ()))}, deferred {deferred} "
                         f"(push {push_deferred}, pull {pull_deferred})")
            if deferred == 0:
                break
            current_deferred = (push_deferred, pull_deferred)
            if previous_deferred is not None and current_deferred == previous_deferred and not new_work:
                stopped = "no progress while deferred items remain"
                break
            previous_deferred = current_deferred
            if progress is not None:
                progress(f"waiting {DRAIN_WAIT_SECONDS} seconds before the next cloud batch")
            wait(DRAIN_WAIT_SECONDS)
    finally:
        if annotations_out is not None:
            try:
                annotations_exported = export_annotations(client, annotations_out)
            except Exception as error:
                annotations_error = str(error)

    return {
        "origin": client.origin,
        "batches": batches,
        "remote": remote,
        "push": {
            "pushed": total_pushed,
            "already_synced": total_synced,
            "from_cloud": total_from_cloud,
            "conflicts": conflicts,
        },
        "pull": {
            "pulled": total_pulled,
            "already_cached": total_cached,
            "released": total_released,
        },
        "deferred": deferred,
        "complete": not conflicts and deferred == 0,
        "stopped": stopped,
        "annotations_in": annotations_uploaded,
        "annotations_out": annotations_exported,
        "annotations_error": annotations_error,
    }
