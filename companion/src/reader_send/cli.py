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
from .common import ReaderError, atomic_write, bounded_read, digest, filename, http_url
from .epub import build_epub
from .extract import extract
from .localsend import Device, discover, endpoint, send
from .network import PublicHTTP, decode_html
from .sources import load_sources
from . import subscriptions as publications
from .x_adapter import api_thread, load_export, thread_item
from .x_browser import DEFAULT_CDP_ENDPOINT, capture_x_url
from . import assist, book_hooks, card, card_autosync, cloud_cli, crossfire as crossfire_module, \
    feed_digest, inbox_card, novels, x_following, collect


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
    x = commands.add_parser("x", help="Convert one X post/thread from your signed-in Chrome, an export, or the official API")
    cache_option(x)
    delivery_options(x)
    x.add_argument("input", help="X/Twitter post URL")
    access = x.add_mutually_exclusive_group(required=True)
    access.add_argument("--export", type=Path, help="official API v2 data/includes JSON export")
    access.add_argument("--api", action="store_true", help="explicitly use X_BEARER_TOKEN and your authorized API quota")
    access.add_argument("--browser", action="store_true",
                        help="read the post from the Chrome window already signed in to X (no API key or export)")
    x.add_argument("--browser-endpoint", default=DEFAULT_CDP_ENDPOINT,
                   help="loopback Chrome DevTools endpoint used by --browser (default 127.0.0.1:9333)")
    x.add_argument("--output", "-o", type=Path)
    x.add_argument("--force", action="store_true")
    x.add_argument("--send", action="store_true")
    x.add_argument("--no-images", action="store_true")
    x.add_argument("--remote-images", action="store_true", help="fetch images from a JSON export (otherwise offline)")
    following = commands.add_parser("x-following", help="Collect the newest posts from your X Following timeline")
    cache_option(following)
    following_operations = following.add_subparsers(dest="following_operation", required=True)
    following_sync = following_operations.add_parser("sync", help="Read the Following tab once and store new posts")
    cache_option(following_sync)
    delivery_options(following_sync)
    following_sync.add_argument("--since-hours", type=int, default=24,
                                help="only posts newer than this many hours (1–168; default 24)")
    following_sync.add_argument("--limit", type=int, default=50,
                                help="maximum posts stored per pass (1–200; default 50)")
    following_sync.add_argument("--send", action="store_true")
    following_sync.add_argument("--no-images", action="store_true")
    following_sync.add_argument("--browser-endpoint", default=DEFAULT_CDP_ENDPOINT,
                                help="loopback Chrome DevTools endpoint (default 127.0.0.1:9333)")
    publication = commands.add_parser("subscriptions", help="Manage automatic RSS and Mac-local export collection")
    cache_option(publication)
    operations = publication.add_subparsers(dest="operation", required=True)
    add = operations.add_parser("add", help="Add or update a chosen publication")
    cache_option(add)
    add.add_argument("name")
    add.add_argument("--feed", default="", help="public RSS/Atom URL; Substack publications usually use /feed")
    add.add_argument("--export-dir", default="", help="folder of your authorized saved HTML/exports")
    add.add_argument("--browser-url", default="",
                     help="publication archive URL read through an already signed-in Chrome (read-only)")
    listing = operations.add_parser("list")
    cache_option(listing)
    remove = operations.add_parser("remove")
    cache_option(remove)
    remove.add_argument("name")
    sync = operations.add_parser("sync", help="Collect new posts once or periodically into the reading inbox")
    cache_option(sync)
    sync.add_argument("--name", default="")
    sync.add_argument("--limit", type=int, default=10, help="maximum new public posts per publication/pass (1–100)")
    sync.add_argument("--timeout", type=seconds, default=30, help="per-page browser or HTTP request timeout in seconds")
    sync.add_argument("--retries", type=retries, default=2, help="HTTP retries; browser pages are never retried")
    sync.add_argument("--browser-endpoint", default=DEFAULT_CDP_ENDPOINT,
                      help=f"loopback Chrome DevTools endpoint for browser sources (default {DEFAULT_CDP_ENDPOINT})")
    sync.add_argument("--watch", action="store_true", help="keep collecting until interrupted; run only on your Mac")
    sync.add_argument("--interval", type=int, default=900, help="seconds between passes, at least 60 (default 900)")
    sync.add_argument("--dry-run", action="store_true")
    sync.add_argument("--no-images", action="store_true")
    sync.add_argument("--since-days", type=int, default=None,
                      help="only import posts published within this many days (1–365); default off")
    schedule = operations.add_parser("schedule", help="Install the macOS launchd timer for the unified subscriptions + X + site sync collector")
    cache_option(schedule)
    schedule_operations = schedule.add_subparsers(dest="schedule_operation", required=True)
    schedule_install = schedule_operations.add_parser("install", help="Install/reload the 15-minute user LaunchAgent")
    cache_option(schedule_install)
    schedule_install.add_argument("--config", type=Path,
                                  help="card configuration read once at install time to pin the annotation output (default: ~/Library/Application Support/reader-send/card.json)")
    cards = commands.add_parser("card", help="Deliver cached books and reviewed firmware to the configured Mac SD card")
    cache_option(cards)
    card_actions = cards.add_subparsers(dest="card_operation", required=True)
    def config_option(command):
        command.add_argument("--config", type=Path, help="card configuration path (default: ~/Library/Application Support/reader-send/card.json)")
    card_setup = card_actions.add_parser("setup", help="Bind one removable volume UUID, annotation output, reviewed firmware and launchers")
    cache_option(card_setup)
    config_option(card_setup)
    card_setup.add_argument("--volume-uuid", required=True, help="the card's exact VolumeUUID from diskutil info")
    card_setup.add_argument("--annotations-dir", type=Path, required=True, help="Mac folder for annotation Markdown")
    card_setup.add_argument("--firmware", type=Path, required=True, help="reviewed local firmware image")
    card_setup.add_argument("--sha256", required=True, help="expected SHA-256 of that image")
    card_setup.add_argument("--launchers-dir", type=Path, required=True, help="Finder folder for generated .command launchers")
    card_sync = card_actions.add_parser("sync", help="Copy verified cached EPUBs and import annotations; no firmware action")
    cache_option(card_sync)
    config_option(card_sync)
    card_sync.add_argument("--novels", type=Path, metavar="OUT_DIR",
                           help="copy the EPUBs converted by `novels convert` into /books/Novels (default: ~/papyrix/novels-epub when it has a manifest)")
    card_sync.add_argument("--books", type=Path, metavar="DIR",
                           help="copy the EPUBs of one local shelf folder into /books/Books (default: ~/papyrix/books when it exists)")
    card_sync.add_argument("--dictionaries", type=Path, metavar="DIR",
                           help="mirror each StarDict folder in DIR to /dictionaries/<name> when missing or changed (default: ~/papyrix/dictionaries when it exists)")
    card_sync.add_argument("--crossfire", action="store_true",
                           help="after importing annotations, build a Cross-Fire card in /books/Inbox")
    card_status = card_actions.add_parser("status", help="Report whether the exact configured card is mounted; read-only")
    cache_option(card_status)
    config_option(card_status)
    card_autosync_parser = card_actions.add_parser(
        "autosync",
        help="On card plug-in: pull ready cloud books, copy them to /books/Inbox, verify by SHA-256, then acknowledge them")
    cache_option(card_autosync_parser)
    config_option(card_autosync_parser)
    card_autosync_parser.add_argument("--batch", type=int, default=card_autosync.DEFAULT_BATCH,
                                      help=f"downloads per pass (1–{card_autosync.MAX_BATCH}; default {card_autosync.DEFAULT_BATCH}); each pass is also limited to the per-minute request budget")
    card_autosync_parser.add_argument("--passes", type=int, default=card_autosync.DEFAULT_PASSES,
                                      help=f"maximum passes, 60 seconds apart (1–10; default {card_autosync.DEFAULT_PASSES})")
    card_autosync_parser.add_argument("--log", type=Path, default=card_autosync.default_log_path(),
                                      help="append a short run log here (default ~/Library/Logs/reader-card-autosync.log)")
    card_autosync_parser.add_argument("--no-notify", action="store_true", help="do not post a macOS notification")
    card_autosync_parser.add_argument("--crossfire", action="store_true",
                                      help="after importing annotations, build a Cross-Fire card in /books/Inbox")
    for name, description in (
            ("repack", "Plan daily X digests from the single X-post EPUBs in /books/Inbox; --apply writes them and "
                       "moves the singles to /books/Inbox/.singles-archive/ (never deletes)"),
            ("cleanup", "Plan the Inbox auto-delete pass (finished/expired feed items to /books/Inbox/.trash/); "
                        "--apply performs it")):
        maintenance = card_actions.add_parser(name, help=description)
        cache_option(maintenance)
        config_option(maintenance)
        maintenance.add_argument("--root", type=Path,
                                 help="mounted card folder (default: resolve the configured card UUID)")
        maintenance.add_argument("--apply", action="store_true", help="change the card; the default is a dry run")
        if name == "repack":
            maintenance.add_argument("--timezone", default=feed_digest.DEFAULT_TZ,
                                     help=f"local day for grouping (default {feed_digest.DEFAULT_TZ})")
    card_update = card_actions.add_parser("update", help="Stage one reviewed firmware image on the card; never flashes")
    cache_option(card_update)
    config_option(card_update)
    card_update.add_argument("--image", type=Path, help="one explicit replacement image")
    card_update.add_argument("--expected-sha256", help="required with --image")
    card_provision = card_actions.add_parser("provision", help="Write the optional firmware direct-sync config; never stages firmware")
    cache_option(card_provision)
    config_option(card_provision)
    card_provision.add_argument("--force", action="store_true", help="replace an invalid existing direct-sync config")
    card_provision.add_argument("--rotate-key", action="store_true", help="explicitly replace the preserved device key from Keychain")
    direct_sync = card_provision.add_mutually_exclusive_group()
    direct_sync.add_argument("--enabled", dest="enabled", action="store_true", default=None, help="enable firmware direct sync")
    direct_sync.add_argument("--disabled", dest="enabled", action="store_false", help="disable firmware direct sync without changing the key")
    novels_parser = commands.add_parser("novels", help="Convert plain-text Chinese novels into chaptered EPUBs for the reader")
    novels_actions = novels_parser.add_subparsers(dest="novels_operation", required=True)
    novels_convert = novels_actions.add_parser(
        "convert", help="Convert every .txt below SRC_DIR into a mirrored .epub below OUT_DIR; SRC_DIR is only read")
    novels_convert.add_argument("src", type=Path, metavar="SRC_DIR", help="folder of .txt novels (searched recursively, never written)")
    novels_convert.add_argument("out", type=Path, metavar="OUT_DIR", help="folder for the EPUBs, manifest and report (must be outside SRC_DIR)")
    novels_convert.add_argument("--jobs", type=int, choices=range(1, 33), metavar="N", default=novels.default_jobs(),
                                help="parallel conversion processes (default: one fewer than the CPU count, at most 6)")
    novels_convert.add_argument("--force", action="store_true", help="reconvert every book even when its source hash is unchanged")
    novels_convert.add_argument("--rebuild-stale", action="store_true",
                                help="reconvert only books built by an older text handling (missing or older text_clean revision)")
    novels_convert.add_argument("--json", action="store_true", help="print the full per-book report instead of the summary")
    scheduled = commands.add_parser("collect", help="Run recent subscriptions and X Following, then push/pull the private reader site")
    cache_option(scheduled)
    scheduled.add_argument("--annotations-out", type=Path,
                           help="explicit cloud annotation NDJSON path (default: <cache-dir>/annotations/cloud-annotations.ndjson)")
    inbox = commands.add_parser("inbox", help="Update source-stable local read/progress state")
    cache_option(inbox)
    inbox.add_argument("id", help="cached article ID")
    inbox.add_argument("--state", choices=("unread", "reading", "read"), required=True)
    inbox.add_argument("--progress", type=float, help="fraction from 0 to 1; defaults to 0/unread or 1/read")
    hooks = commands.add_parser("hooks", help="Insert a 'Sharpest Passages' front card into a real EPUB book")
    hooks.add_argument("book", type=Path, help="EPUB file (rewritten in place unless --output is given)")
    hooks.add_argument("--output", "-o", type=Path, help="write the hooked copy here instead of replacing the book")
    hooks.add_argument("--count", type=int, default=book_hooks.DEFAULT_COUNT,
                       help=f"passages on the front card (1–12; default {book_hooks.DEFAULT_COUNT})")
    hooks.add_argument("--minimum", type=int, default=book_hooks.DEFAULT_MINIMUM,
                       help=f"refuse the book below this many strong passages (default {book_hooks.DEFAULT_MINIMUM})")
    hooks.add_argument("--no-backup", action="store_true", help="do not keep a .pre-hooks.epub copy of the original")
    hooks.add_argument("--dry-run", action="store_true", help="print the selected passages; write nothing")
    crossfire = commands.add_parser(
        "crossfire", help="Pair imported reader annotations with Substack replies and build an Inbox card")
    crossfire.add_argument("--annotations", type=Path,
                           help="annotation JSON/NDJSON file or directory (default: the mounted card's store)")
    crossfire.add_argument("--config", type=Path, help="card configuration used to find the attached card")
    crossfire.add_argument("--corpus", type=Path, help="Substack Markdown archive (default ~/substack-corpus)")
    crossfire.add_argument("--neighbors", type=Path,
                           help="neighbour JSONL folder (default ~/substack-neighbors/posts)")
    crossfire.add_argument("--inbox", type=Path, help="output folder for the card (default <card>/books/Inbox)")
    crossfire.add_argument("--out", type=Path, help="one explicit output EPUB path")
    crossfire.add_argument("--db", type=Path, help="SQLite FTS5 index cache path")
    crossfire.add_argument("--replies", type=int, default=crossfire_module.DEFAULT_REPLIES,
                           help=f"contrasting passages per annotation (1–6; default {crossfire_module.DEFAULT_REPLIES})")
    crossfire.add_argument("--limit", type=int, default=crossfire_module.DEFAULT_CANDIDATES,
                           help="candidate chunks ranked per query (1–5000)")
    crossfire.add_argument("--dry-run", action="store_true", help="print matches; write nothing")
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
    if args.export:
        payload = load_export(bounded_read(args.export.expanduser(), 8 * 1024 * 1024))
    elif args.browser:
        payload = capture_x_url(args.input, endpoint=args.browser_endpoint, timeout=args.timeout)
    else:
        payload = api_thread(args.input, args.timeout)
    item = thread_item(args.input, payload)
    if args.browser:
        item.warnings.extend(payload.get("_warnings", []))
        item.provenance["capture"] = "chrome-cdp"
    article_id = cache.key(item.source_id, {"no_images": args.no_images})
    name = filename(item.title, article_id)
    destination = output_path(args, name, False)
    if args.dry_run:
        emit({"id": article_id, **item.metadata(), "path": str(destination or cache.directory(article_id) / name),
              "dry_run": True, "would_send": args.send})
        return 0
    if destination and destination.exists() and not args.force:
        raise ReaderError(f"Output exists: {destination}; use --force to replace it")
    data = build_epub(item, article_id, PublicHTTP(args.timeout), remote_images=args.api or args.remote_images or args.browser,
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


def x_following_command(args, cache: Cache) -> int:
    if args.reader and not args.send:
        raise ReaderError("--reader requires --send on the x-following command")
    if args.send and not args.dry_run:
        device = select_reader(args)

        def deliver(path):
            return send(path, device, timeout=args.timeout)
    else:
        deliver = None
    result = x_following.sync(cache, PublicHTTP(args.timeout), since_hours=args.since_hours,
                              limit=args.limit, dry_run=args.dry_run, no_images=args.no_images,
                              endpoint=args.browser_endpoint, timeout=args.timeout, deliver=deliver)
    for warning in result["warnings"]:
        print(f"reader-send: warning: {warning}", file=sys.stderr)
    for error in result["errors"]:
        print(f"reader-send: error: {error.get('stage', 'x-following')}: {error.get('error', '')}", file=sys.stderr)
    emit(result)
    return 2 if result["errors"] else 0


def subscription_command(args, cache: Cache) -> int:
    if args.operation == "add":
        emit(publications.add(cache, args.name, args.feed, args.export_dir,
                              browser_url=args.browser_url))
        return 0
    if args.operation == "schedule":
        emit(collect.install_launchd(cache, card_config=args.config))
        return 0
    if args.operation == "list":
        emit(publications.subscriptions(cache, include_disabled=True))
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
        managed_refresh = None
        if not args.dry_run and args.since_days is not None:
            try:
                managed_refresh = publications.refresh_managed_subscriptions(
                    cache, endpoint=args.browser_endpoint, timeout=args.timeout)
            except (ReaderError, OSError) as exc:
                managed_refresh = {"status": "error", "source": "substack-account", "error": str(exc)}
        client = PublicHTTP(args.timeout, args.retries)
        if args.dry_run:
            result = publications.sync(cache, client, name=args.name, limit=args.limit,
                                       dry_run=True, no_images=args.no_images,
                                       browser_endpoint=args.browser_endpoint, timeout=args.timeout,
                                       since_days=args.since_days)
        else:
            with publications.inbox_lock(cache):
                result = publications.sync(cache, client, name=args.name, limit=args.limit,
                                           no_images=args.no_images,
                                           browser_endpoint=args.browser_endpoint, timeout=args.timeout,
                                           since_days=args.since_days)
        if managed_refresh is not None:
            result["managed_refresh"] = managed_refresh
            if managed_refresh.get("status") == "error":
                result["errors"].append({"stage": "managed-refresh", "error": managed_refresh["error"]})
        emit(result)
        if not args.watch:
            return 2 if result["errors"] else 0
        time.sleep(args.interval)


def inbox_command(args, cache: Cache, config_path) -> int:
    """card repack / card cleanup: dry runs by default, on --root or the configured card."""
    if args.root is not None:
        root = args.root.expanduser().resolve()
        if not (root / inbox_card.INBOX).is_dir():
            raise ReaderError(f"{root} has no books/Inbox folder")
        state_key = "root-" + digest(str(root))[:16]
    else:
        volume = card.resolve_card(card.load_config(config_path)["volume_uuid"])
        root, state_key = volume.root, volume.volume_uuid
    if args.card_operation == "repack":
        plan = inbox_card.plan_repack(root, args.timezone)
        result = {"dry_run": not args.apply, **inbox_card.describe_repack(plan)}
        if args.apply:
            result["applied"] = inbox_card.apply_repack(root, plan)
        emit(result)
        return 0
    emit(inbox_card.run_cleanup(root, inbox_card.state_path(cache.root, state_key), apply=args.apply))
    return 0


def card_command(args, cache: Cache) -> int:
    config_path = args.config or card.default_config_path()
    if args.card_operation == "setup":
        emit(card.setup(cache, config_path, volume_uuid=args.volume_uuid,
                        annotations_dir=args.annotations_dir, firmware_path=args.firmware,
                        firmware_sha256=args.sha256, launchers_dir=args.launchers_dir))
        return 0
    if args.card_operation in ("repack", "cleanup"):
        return inbox_command(args, cache, config_path)
    config = card.load_config(config_path)
    if args.card_operation == "status":
        result, mounted = card.card_status(config)
        emit(result)
        return 0 if mounted else 1
    if args.card_operation == "sync":
        shelves = card.default_shelves()
        result = card.sync_card(
            cache, config,
            novels_dir=args.novels if args.novels is not None else shelves.get("novels"),
            books_dir=args.books if args.books is not None else shelves.get("books"),
            dictionaries_dir=args.dictionaries if args.dictionaries is not None else shelves.get("dictionaries"),
            crossfire=True if args.crossfire else None)
        failures = [*result.get("novels", {}).get("errors", []),
                    *result.get("books", {}).get("shelf", {}).get("errors", []),
                    *result.get("dictionaries", {}).get("errors", [])]
        emit(result)
        return 2 if failures else 0
    if args.card_operation == "autosync":
        shelves = card.default_shelves()
        result = card_autosync.autosync(
            cache, config, batch=args.batch, passes=args.passes, log=card_autosync.RunLog(args.log),
            notify=None if args.no_notify else card_autosync.macos_notification,
            novels_dir=shelves.get("novels"), books_dir=shelves.get("books"),
            dictionaries_dir=shelves.get("dictionaries"),
            crossfire=True if args.crossfire else None)
        emit(result)
        return 2 if result["status"] in {"error", "rate_limited"} else 0
    if args.card_operation == "provision":
        emit(card.provision_reader_sync(config, force=args.force, rotate_key=args.rotate_key,
                                        enabled=args.enabled))
        return 0
    emit(card.stage_firmware(config, image=args.image, expected_sha256=args.expected_sha256))
    return 0


def novels_command(args) -> int:
    def progress(done: int, total: int, record: dict) -> None:
        if record["status"] == "failed":
            detail = record["error"]
        elif record["status"] == "skipped":
            detail = "unchanged"
        else:
            detail = f"{record.get('mode')}, {record.get('chapters', 0)} chapters, {record.get('files', 0)} files"
        print(f"[{done}/{total}] {record['status']}: {record['source']} ({detail})", file=sys.stderr, flush=True)

    report = novels.convert_tree(args.src, args.out, force=args.force, jobs=args.jobs,
                                 rebuild_stale=args.rebuild_stale, progress=progress)
    failures = [{"source": b["source"], "error": b["error"]} for b in report["books"] if b["status"] == "failed"]
    emit(report if args.json else {"summary": report["summary"], "failures": failures})
    return 2 if failures else 0


def hooks_command(args) -> int:
    if args.dry_run:
        passages = book_hooks.select_passages(args.book, count=args.count, minimum=args.minimum)
        emit({"book": str(args.book), "passages": [passage.as_dict() for passage in passages]})
        return 0
    result = book_hooks.apply_hooks(args.book, args.output, count=args.count,
                                    minimum=args.minimum, backup=not args.no_backup)
    emit(result.as_dict())
    return 0


def run(args) -> int:
    if args.command == "assist":
        emit(assist.run(args))
        return 0
    if args.command == "discover":
        emit([d.metadata() for d in discover(args.discovery_timeout, args.interface)])
        return 0
    if args.command == "novels":
        return novels_command(args)
    if args.command == "hooks":
        return hooks_command(args)
    if args.command == "crossfire":
        emit(crossfire_module.run(args))
        return 0
    cache = Cache(args.cache_dir)
    if args.command == "card":
        return card_command(args, cache)
    if args.command == "collect":
        result = collect.collect(cache, annotations_out=args.annotations_out)
        emit(result)
        return 0 if result.get("ok") else 2
    if args.command == "cloud":
        emit(cloud_cli.run(args, cache))
        return 0
    if args.command in ("fetch", "convert"):
        return convert_command(args, cache)
    if args.command == "x":
        return x_command(args, cache)
    if args.command == "x-following":
        return x_following_command(args, cache)
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
