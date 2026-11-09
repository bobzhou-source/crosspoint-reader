from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .cache import Cache
from .cloud import DEFAULT_ORIGIN, CloudClient, json_bytes, origin_url, prepare_push
from .cloud_annotations import (annotation_batches, download_annotations, load_annotations,
                                upload_annotations)
from .common import ReaderError, atomic_write
from . import cloud_drain, cloud_links, cloud_sync


def add_parser(commands, cache_option, seconds):
    cloud = commands.add_parser("cloud", help="Explicit opt-in private HTTPS EPUB/annotation sync (READER_SYNC_KEY environment only)")
    cache_option(cloud)

    def options(command, *, defaults=False):
        command.add_argument("--origin", default=DEFAULT_ORIGIN if defaults else argparse.SUPPRESS,
                             help="explicit HTTPS origin, default $READER_SYNC_ORIGIN or https://example.com; no redirects")
        command.add_argument("--timeout", type=seconds, default=30 if defaults else argparse.SUPPRESS)
        if not defaults:
            cache_option(command)

    options(cloud, defaults=True)
    operations = cloud.add_subparsers(dest="cloud_operation", required=True)
    sync = operations.add_parser("sync", help="One explicit bounded pass: push cache, pull ready books and optionally move annotations")
    options(sync)
    sync.add_argument("--drain", action="store_true",
                      help="repeat bounded batches 60 seconds apart until deferred work is empty; annotation input once, output once at the end")
    sync.add_argument("--incoming-dir", type=Path, help="keep pulled UUID-named files here instead of installing cache entries")
    sync.add_argument("--annotations-in", type=Path, help="upload native card JSON/directory or API/NDJSON annotations")
    sync.add_argument("--annotations-out", type=Path, help="write all server annotations here (.ndjson/.jsonl for NDJSON)")
    sync.add_argument("--device-id", help="stable nonsecret device label required for native card annotations")
    sync.add_argument("--item-id", help="optional cloud item UUID for uploaded annotations")
    sync.add_argument("--max-push", type=int, default=cloud_sync.DEFAULT_MAX_ITEMS,
                      help=f"maximum pushes in this pass (1–64; default {cloud_sync.DEFAULT_MAX_ITEMS})")
    sync.add_argument("--max-pull", type=int, default=cloud_sync.DEFAULT_MAX_ITEMS,
                      help=f"maximum pulls in this pass (1–64; default {cloud_sync.DEFAULT_MAX_ITEMS})")
    sync.add_argument("--no-push", action="store_true", help="do not upload local cache entries")
    sync.add_argument("--no-pull", action="store_true", help="do not download ready cloud books")
    sync.add_argument("--dry-run", action="store_true", help="local validation plus one read-only listing; no writes")
    push = operations.add_parser("push", help="Upload one already-converted cache entry, without altering its EPUB")
    options(push)
    push.add_argument("id", help="full 24-character local cache ID")
    push.add_argument("--source-path", help="stable intended reader path; default cached EPUB filename (never the Mac cache path)")
    push.add_argument("--dry-run", action="store_true", help="validate/hash locally; no network, writes or secret required")
    listing = operations.add_parser("list", help="List ready EPUBs")
    options(listing)
    listing.add_argument("--all", action="store_true", help="include uploading and consumed entries")
    pull = operations.add_parser("pull", help="Download verified EPUB bytes plus original metadata; does not ack")
    options(pull)
    pull.add_argument("id", help="cloud item UUID")
    pull.add_argument("--output-dir", type=Path, required=True)
    pull.add_argument("--force", action="store_true", help="replace existing UUID-named output files")
    for name, help_text in (("ack", "Mark a completed cloud item consumed; frees all server EPUB bytes, keeps metadata receipt"),
                            ("delete", "Explicitly delete a cloud item/blob; annotations survive")):
        operation = operations.add_parser(name, help=help_text)
        options(operation)
        operation.add_argument("id", help="cloud item UUID")
    annotations = operations.add_parser("annotations", help="Upload/download annotations or export full NDJSON")
    options(annotations)
    actions = annotations.add_subparsers(dest="annotation_operation", required=True)
    upload = actions.add_parser("upload", help="Upload API JSON/NDJSON or native annotation JSON/directory")
    options(upload)
    upload.add_argument("input", type=Path)
    upload.add_argument("--device-id", help="stable nonsecret device name, required for native book-local IDs")
    upload.add_argument("--item-id", help="optional cloud item UUID; not required for LocalSend/USB books")
    upload.add_argument("--dry-run", action="store_true", help="validate every record and batch locally; no network, writes or secret required")
    for name in ("download", "export"):
        action = actions.add_parser(name, help="Download all pages as " + ("JSON" if name == "download" else "unchanged NDJSON"))
        options(action)
        action.add_argument("--after", type=int, default=0, help="exclusive server receive-sequence cursor")
        action.add_argument("--limit", type=int, default=100, help="records per request, 1–100 (all pages are fetched)")
        action.add_argument("--output", "-o", type=Path, required=name == "export",
                            help="private output file; JSON download otherwise goes to stdout")
        action.add_argument("--force", action="store_true")
    links = operations.add_parser("links", help="List or drain the private shared-link queue")
    options(links)
    link_actions = links.add_subparsers(dest="link_operation", required=True)
    for name, help_text in (("list", "List pending shared links"),
                            ("drain", "Fetch, verify, push and acknowledge a bounded shared-link batch")):
        action = link_actions.add_parser(name, help=help_text)
        options(action)
        action.add_argument("--limit", type=int, default=cloud_links.DEFAULT_LINK_LIMIT,
                            help=f"pending links per pass (1-64; default {cloud_links.DEFAULT_LINK_LIMIT})")


def run(args, cache: Cache) -> dict:
    origin = origin_url(args.origin)
    if args.cloud_operation == "sync":
        arguments = {
            "incoming_dir": args.incoming_dir,
            "annotations_in": args.annotations_in, "annotations_out": args.annotations_out,
            "device_id": args.device_id, "item_id": args.item_id,
            "push": not args.no_push, "pull": not args.no_pull,
            "max_push": args.max_push, "max_pull": args.max_pull, "dry_run": args.dry_run,
        }
        client = CloudClient(origin, args.timeout)
        if args.drain:
            def progress(message):
                print(f"reader-send: {message}", file=sys.stderr, flush=True)
            return cloud_drain.drain(client, cache, progress=progress, **arguments)
        return cloud_sync.sync(client, cache, **arguments)
    if args.cloud_operation == "links":
        client = CloudClient(origin, args.timeout)
        if args.link_operation == "list":
            return {"links": client.links(limit=args.limit)}
        return cloud_links.drain(client, cache, limit=args.limit)
    if args.cloud_operation == "push":
        metadata, data = prepare_push(cache, args.id, args.source_path)
        if args.dry_run:
            return {"dry_run": True, "origin": origin, "item": metadata,
                    "requests": ["POST /reader/items", f"PUT /reader/items/{metadata['id']}/content"]}
        return {"item": CloudClient(origin, args.timeout).push(metadata, data)}
    if args.cloud_operation == "annotations" and args.annotation_operation == "upload":
        batches = annotation_batches(load_annotations(args.input), device_id=args.device_id, item_id=args.item_id)
        if args.dry_run:
            return {"dry_run": True, "origin": origin, "batches": len(batches),
                    "body_bytes": [len(body) for body in batches]}
        return upload_annotations(CloudClient(origin, args.timeout), batches)
    if args.cloud_operation == "annotations" and args.output:
        output = args.output.expanduser()
        if not args.force and (output.exists() or output.is_symlink()):
            raise ReaderError("Annotation output exists; choose another path or use --force")
    client = CloudClient(origin, args.timeout)
    if args.cloud_operation == "list":
        return {"items": client.items(ready_only=not args.all, include_all=args.all)}
    if args.cloud_operation == "pull":
        return client.pull(args.id, args.output_dir, overwrite=args.force)
    if args.cloud_operation == "ack":
        return {"item": client.ack(args.id)}
    if args.cloud_operation == "delete":
        return client.delete(args.id)
    result = download_annotations(client, after=args.after, limit=args.limit, export=args.annotation_operation == "export")
    if args.output:
        data = result.pop("ndjson") if args.annotation_operation == "export" else json_bytes(result) + b"\n"
        atomic_write(args.output.expanduser(), data, overwrite=args.force)
        return {"output": str(args.output.expanduser()), "count": len(result["annotations"]), "after": result["after"]}
    return result
