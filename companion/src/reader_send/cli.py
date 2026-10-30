from __future__ import annotations

import argparse
import json
import math
import sys
import time
import zipfile
from pathlib import Path

from . import __version__
from .cache import ID_PATTERN, Cache, default_cache
from .common import ReaderError, atomic_write, bounded_read, filename, http_url
from .epub import build_epub
from .extract import extract
from .localsend import Device, discover, endpoint, send
from .network import PublicHTTP, decode_html
from .sources import load_sources
from . import subscriptions as publications
from .x_adapter import api_thread, load_export, thread_item
from . import assist, cloud_cli


def seconds(value: str) -> float:
    try:
        number = float(value)
        if not math.isfinite(number) or not 0 < number <= 300:
            raise ValueError
        return number
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a number greater than 0 and at most 300") from exc


def retries(value: str) -> int:
    try:
        number = int(value)
        if not 0 <= number <= 5:
            raise ValueError
        return number
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be an integer from 0 to 5") from exc


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(prog="reader-send", description="Public articles / local HTML → EPUB → LocalSend reader. No accounts or cookies.")
    root.add_argument("--version", action="version", version=__version__)
    root.add_argument("--cache-dir", type=Path, default=default_cache())
    commands = root.add_subparsers(dest="command", required=True)
    def cache_option(command):
        command.add_argument("--cache-dir", type=Path, default=argparse.SUPPRESS, help="article cache directory")
    def delivery_options(command):
        command.add_argument("--reader", help="explicit HTTP reader HOST[:PORT], default port 53317")
        command.add_argument("--discovery-timeout", type=seconds, default=3.0)
        command.add_argument("--interface", default="0.0.0.0", help="IPv4 LAN address for multicast discovery")
        command.add_argument("--timeout", type=seconds, default=30, help="socket timeout in seconds (default 30)")
        command.add_argument("--dry-run", action="store_true", help="do not write files or send; fetch may read public HTTP")
    for name, description in (("fetch", "Fetch one public Substack or generic article URL"), ("convert", "Convert local HTML, a post directory, or Substack ZIP export")):
        command = commands.add_parser(name, help=description)
        command.add_argument("input", help="HTTP(S) URL" if name == "fetch" else "HTML / directory / ZIP path")
        cache_option(command)
        delivery_options(command)
        command.add_argument("--output", "-o", type=Path, help="EPUB filename, or output directory (required for multiple files only if exporting)")
        command.add_argument("--force", action="store_true", help="replace an existing explicit output file")
        command.add_argument("--title", default="", help="override extracted title")
        command.add_argument("--author", default="", help="override extracted author")
        command.add_argument("--date", default="", help="override publication date")
        command.add_argument("--no-images", action="store_true", help="omit images, retaining alt text")
        command.add_argument("--retries", type=retries, default=2, help="public HTTP GET retries only (0–5; default 2)")
        command.add_argument("--send", action="store_true", help="send after conversion (otherwise only create EPUB)")
        if name == "fetch":
            command.add_argument("--refresh", action="store_true", help="refetch instead of reusing a cached EPUB")
        else:
            command.add_argument("--source-url", default="", help="article source URL for local HTML / relative links")
            command.add_argument("--remote-images", action="store_true", help="allow public HTTP images; local conversion is offline by default")
    inspect = commands.add_parser("inspect", help="List cached articles or inspect a full cache ID")
    cache_option(inspect)
    inspect.add_argument("id", nargs="?")
    inspect.add_argument("--json", action="store_true", help="emit structured cache metadata")
    discovery = commands.add_parser("discover", help="List LocalSend v2 devices on the LAN")
    discovery.add_argument("--discovery-timeout", type=seconds, default=3.0)
    discovery.add_argument("--interface", default="0.0.0.0")
    sender = commands.add_parser("send", help="Send an existing EPUB or cached article ID")
    cache_option(sender)
    sender.add_argument("input", help="EPUB path or full cached article ID")
    delivery_options(sender)
    x = commands.add_parser("x", help="Convert one X post/thread from an authorized JSON export or official API")
    cache_option(x)
    delivery_options(x)
    x.add_argument("input", help="X/Twitter post URL")
    access = x.add_mutually_exclusive_group(required=True)
    access.add_argument("--export", type=Path, help="official API v2 data/includes JSON export")
    access.add_argument("--api", action="store_true", help="explicitly use X_BEARER_TOKEN and your authorized API quota")
    x.add_argument("--output", "-o", type=Path)
    x.add_argument("--force", action="store_true")
    x.add_argument("--send", action="store_true")
    x.add_argument("--no-images", action="store_true")
    x.add_argument("--remote-images", action="store_true", help="fetch images from a JSON export (otherwise offline)")
    publication = commands.add_parser("subscriptions", help="Manage automatic RSS and Mac-local export collection")
    cache_option(publication)
    operations = publication.add_subparsers(dest="operation", required=True)
    add = operations.add_parser("add", help="Add or update a chosen publication")
    cache_option(add)
    add.add_argument("name")
    add.add_argument("--feed", default="", help="public RSS/Atom URL; Substack publications usually use /feed")
    add.add_argument("--export-dir", default="", help="folder of your authorized saved HTML/exports")
    listing = operations.add_parser("list")
    cache_option(listing)
    remove = operations.add_parser("remove")
    cache_option(remove)
    remove.add_argument("name")
    sync = operations.add_parser("sync", help="Collect new posts once or periodically into the reading inbox")
    cache_option(sync)
    sync.add_argument("--name", default="")
    sync.add_argument("--limit", type=int, default=10, help="maximum new public posts per publication/pass (1–100)")
    sync.add_argument("--timeout", type=seconds, default=30)
    sync.add_argument("--retries", type=retries, default=2)
    sync.add_argument("--watch", action="store_true", help="keep collecting until interrupted; run only on your Mac")
    sync.add_argument("--interval", type=int, default=900, help="seconds between passes, at least 60 (default 900)")
    sync.add_argument("--dry-run", action="store_true")
    sync.add_argument("--no-images", action="store_true")
    inbox = commands.add_parser("inbox", help="Update source-stable local read/progress state")
    cache_option(inbox)
    inbox.add_argument("id", help="cached article ID")
    inbox.add_argument("--state", choices=("unread", "reading", "read"), required=True)
    inbox.add_argument("--progress", type=float, help="fraction from 0 to 1; defaults to 0/unread or 1/read")
    cloud_cli.add_parser(commands, cache_option, seconds)
    assist.add_parser(commands, seconds)
    return root


def select_reader(args) -> Device:
    if args.reader:
        return endpoint(args.reader)
    devices = [d for d in discover(args.discovery_timeout, args.interface) if d.protocol == "http"]
    if not devices:
        raise ReaderError("No HTTP LocalSend reader found. Open LocalSend on the reader, join the same LAN, or use --reader HOST:53317")
    if len(devices) != 1:
        choices = ", ".join(f"{d.alias} ({d.address})" for d in devices)
        raise ReaderError(f"Multiple LocalSend devices found: {choices}. Select one with --reader HOST:PORT")
    return devices[0]


def output_path(args, name: str, multiple: bool) -> Path | None:
    if args.output is None:
        return None
    output = args.output.expanduser()
    if output.suffix.lower() == ".epub" and not output.is_dir():
        if multiple:
            raise ReaderError("Multiple posts require an output directory, not one .epub filename")
        return output
    return output / name


def emit(value) -> None:
    print(json.dumps(value, ensure_ascii=False, indent=2), flush=True)


def convert_command(args, cache: Cache) -> int:
    if args.reader and not args.send:
        raise ReaderError("--reader selects a delivery target; add --send or use the send command")
    if args.command == "convert" and args.source_url:
        args.source_url = http_url(args.source_url)
    client = PublicHTTP(timeout=args.timeout, retries=args.retries)
    options = {"title": args.title, "author": args.author, "date": args.date, "no_images": args.no_images}
    results = []
    device = None
    if args.send and args.reader:
        device = endpoint(args.reader)
    if args.command == "fetch":
        url = http_url(args.input)
        article_id = cache.key(url, options)
        cached = None if args.refresh else cache.get(article_id)
        if cached:
            sources = [(article_id, None, None, cached)]
        else:
            response = client.get(url)
            if response.content_type not in ("text/html", "application/xhtml+xml", "text/plain", "application/octet-stream"):
                raise ReaderError(f"URL returned {response.content_type}, not an HTML article")
            article = extract(decode_html(response.data, response.charset), response.url, title=args.title, author=args.author, date=args.date)
            sources = [(article_id, article, None, None)]
        multiple = False
    else:
        def local_articles():
            for local in load_sources(Path(args.input)):
                metadata = local.metadata
                source_url = args.source_url or metadata.get("source_url", "")
                article_id = cache.key(local.identity, {**options, "source_url": source_url, "remote_images": args.remote_images})
                article = extract(decode_html(local.data), source_url, title=args.title or metadata.get("title", ""),
                                  author=args.author or metadata.get("author", ""), date=args.date or metadata.get("date", ""))
                article.source_type = "substack-export" if local.archive or "substack" in article.source_url else "local"
                if not article.source_url:
                    article.source_id = "local:" + str(local.path or local.archive) + "#" + local.member
                yield article_id, article, local, None
        sources = local_articles()
        multiple = Path(args.input).expanduser().is_dir() or Path(args.input).suffix.lower() == ".zip"
        if multiple and args.output and args.output.suffix.lower() == ".epub" and not args.output.is_dir():
            raise ReaderError("Directory/ZIP inputs require an output directory rather than a single .epub path")
    for article_id, article, local, cached in sources:
        name = cached["filename"] if cached else filename(article.title, article_id)
        destination = output_path(args, name, multiple)
        if args.dry_run:
            record = cached or {"id": article_id, **article.metadata(), "filename": name}
            planned = destination or cache.directory(article_id) / name
            results.append({**record, "path": str(planned), "dry_run": True, "cached": bool(cached), "would_send": args.send,
                            "reader": device.address if device else "discovery" if args.send else None})
            continue
        if destination and destination.exists() and not args.force:
            raise ReaderError(f"Output exists: {destination}; choose another path or use --force")
        if cached:
            record = cached
            data = bounded_read(Path(cached["path"]), 256 * 1024 * 1024) if destination else None
        else:
            data = build_epub(article, article_id, client, local=local,
                              remote_images=args.command == "fetch" or args.remote_images, no_images=args.no_images)
            record = cache.store(article_id, article, data)
        path = Path(record["path"])
        if destination:
            atomic_write(destination, data, overwrite=args.force)
            path = destination
        result = {**record, "output": str(path), "cached": bool(cached)}
        for warning in record.get("warnings", []):
            print(f"reader-send: warning: {warning}", file=sys.stderr)
        if args.send:
            if device is None:
                device = select_reader(args)
            result["delivery"] = send(path, device, timeout=args.timeout)
        results.append(result)
    emit(results[0] if len(results) == 1 else results)
    return 0


def x_command(args, cache: Cache) -> int:
    if args.reader and not args.send:
        raise ReaderError("--reader requires --send on the x command")
    payload = load_export(bounded_read(args.export.expanduser(), 8 * 1024 * 1024)) if args.export else api_thread(args.input, args.timeout)
    item = thread_item(args.input, payload)
    article_id = cache.key(item.source_id, {"no_images": args.no_images})
    name = filename(item.title, article_id)
    destination = output_path(args, name, False)
    if args.dry_run:
        emit({"id": article_id, **item.metadata(), "path": str(destination or cache.directory(article_id) / name),
              "dry_run": True, "would_send": args.send})
        return 0
    if destination and destination.exists() and not args.force:
        raise ReaderError(f"Output exists: {destination}; use --force to replace it")
    data = build_epub(item, article_id, PublicHTTP(args.timeout), remote_images=args.api or args.remote_images,
                      no_images=args.no_images)
    record = cache.store(article_id, item, data)
    path = Path(record["path"])
    if destination:
        atomic_write(destination, data, overwrite=args.force)
        path = destination
    for warning in item.warnings:
        print(f"reader-send: warning: {warning}", file=sys.stderr)
    result = {**record, "output": str(path)}
    if args.send:
        result["delivery"] = send(path, select_reader(args), timeout=args.timeout)
    emit(result)
    return 0


def subscription_command(args, cache: Cache) -> int:
    if args.operation == "add":
        emit(publications.add(cache, args.name, args.feed, args.export_dir))
        return 0
    if args.operation == "list":
        emit(publications.subscriptions(cache))
        return 0
    if args.operation == "remove":
        publications.remove(cache, args.name)
        emit({"removed": args.name})
        return 0
    if not 1 <= args.limit <= 100 or args.interval < 60:
        raise ReaderError("--limit must be 1–100 and --interval at least 60 seconds")
    if args.watch and args.dry_run:
        raise ReaderError("Use a single --dry-run before starting --watch")
    while True:
        client = PublicHTTP(args.timeout, args.retries)
        if args.dry_run:
            result = publications.sync(cache, client, name=args.name, limit=args.limit,
                                       dry_run=True, no_images=args.no_images)
        else:
            with publications.inbox_lock(cache):
                result = publications.sync(cache, client, name=args.name, limit=args.limit,
                                           no_images=args.no_images)
        emit(result)
        if not args.watch:
            return 2 if result["errors"] else 0
        time.sleep(args.interval)


def run(args) -> int:
    if args.command == "assist":
        emit(assist.run(args))
        return 0
    if args.command == "discover":
        emit([d.metadata() for d in discover(args.discovery_timeout, args.interface)])
        return 0
    cache = Cache(args.cache_dir)
    if args.command == "cloud":
        emit(cloud_cli.run(args, cache))
        return 0
    if args.command in ("fetch", "convert"):
        return convert_command(args, cache)
    if args.command == "x":
        return x_command(args, cache)
    if args.command == "subscriptions":
        return subscription_command(args, cache)
    if args.command == "inbox":
        progress = args.progress if args.progress is not None else 1.0 if args.state == "read" else 0.0
        emit(cache.mark(args.id, args.state, progress))
        return 0
    if args.command == "inspect":
        if args.id:
            record = cache.get(args.id)
            if not record:
                raise ReaderError(f"No cached article with ID {args.id}")
            emit(record)
        else:
            entries = cache.entries()
            if args.json:
                emit(entries)
            elif not entries:
                print("No cached articles. Use reader-send fetch URL or reader-send convert FILE.html.")
            else:
                for record in entries:
                    print(f"{record['id']}  {record.get('title', 'Untitled')}\n  {record['path']}")
        return 0
    if args.command == "send":
        path = Path(args.input).expanduser()
        if ID_PATTERN.fullmatch(args.input):
            record = cache.get(args.input)
            if not record:
                raise ReaderError(f"No cached article with ID {args.input}")
            path = Path(record["path"])
        if not path.is_file():
            raise ReaderError(f"EPUB file not found: {path}")
        device = endpoint(args.reader) if args.reader else None
        if not args.dry_run and device is None:
            device = select_reader(args)
        emit(send(path, device, timeout=args.timeout, dry_run=args.dry_run))
        return 0
    raise AssertionError("unknown command")


def main(argv: list[str] | None = None) -> int:
    try:
        return run(parser().parse_args(argv))
    except (ReaderError, OSError, zipfile.BadZipFile, zipfile.LargeZipFile) as exc:
        print(f"reader-send: error: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("reader-send: interrupted", file=sys.stderr)
        return 130
