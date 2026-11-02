#!/usr/bin/env python3
"""Run real firmware protocol/TLS/hash/install paths against an isolated HTTPS peer.

Only Arduino TCP/timing, HAL filesystem and crypto ABI are adapted. The actual
SecureClient, SecureHttpClient, pinned wolfSSL, ReaderSyncProtocol, persistence
and canonical mapper are compiled. No production server, credentials or SD used.
"""
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit
import base64
import hashlib
import hmac
import io
import json
import os
import secrets
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import uuid
import zipfile

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
SECURE = ROOT / "freeink-sdk/libs/network/SecureNet"
TLS = ROOT / "test/tls_verification"
DEPS = next((p for p in (ROOT / ".pio/libdeps/x4pro-gh_release", ROOT / ".pio/libdeps/x4pro")
             if (p / "Arduino-wolfSSL/src").is_dir() and (p / "ArduinoJson/src").is_dir()), None)
if DEPS is None:
    raise SystemExit("Install x4pro-gh_release PlatformIO dependencies first")
WOLF = DEPS / "Arduino-wolfSSL/src"
sys.path.insert(0, str(ROOT / "companion/src"))
from reader_send.cloud_annotations import map_annotation


def command(*args, **kw):
    return subprocess.run([str(x) for x in args], check=True, **kw)


def certificate(key, issuer_key, issuer, name, ca=False):
    now = datetime.now(timezone.utc)
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)])
    builder = (x509.CertificateBuilder().subject_name(subject).issuer_name(issuer or subject)
               .public_key(key.public_key()).serial_number(x509.random_serial_number())
               .not_valid_before(now - timedelta(days=1)).not_valid_after(now + timedelta(days=2))
               .add_extension(x509.BasicConstraints(ca=ca, path_length=0 if ca else None), True)
               .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), False))
    if not ca:
        builder = (builder.add_extension(x509.SubjectAlternativeName([x509.DNSName("localhost")]), False)
                   .add_extension(x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]), False))
    return builder.sign(issuer_key, hashes.SHA256())


def epub(size=256):
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_STORED) as book:
        book.writestr("mimetype", "application/epub+zip")
        book.writestr("content.txt", "x" * max(0, size - 234))
    return out.getvalue()


class State:
    def __init__(self, root, key, payload, mode="chunked", items=1):
        self.root, self.key, self.payload, self.mode = root, key, payload, mode
        self.items = []
        for i in range(items):
            ident = str(uuid.UUID(int=i + 1))
            source = f"/books/Inbox/article-{i}.epub"
            self.items.append(dict(id=ident, title="Article", original_url="https://example.org/", source_path=source,
                                   sha256=hashlib.sha256(payload).hexdigest(), size=len(payload), created_at=i,
                                   consumed_at=None, state="ready"))
        self.acks, self.gets, self.nonces, self.posts, self.errors, self.paths = [], [], set(), [], [], []


with tempfile.TemporaryDirectory(prefix="reader-sync-protocol-") as scratch:
    work = Path(scratch)
    command("python3", SECURE / "tools/build_ca_bundle.py", SECURE / "certs/public-roots.pem", work / "TrustAnchors.generated.h")
    flags = ["-O1", "-DWOLFSSL_USER_SETTINGS", "-DWOLFSSL_OPTIONS_H", "-I" + str(TLS / "stubs"), "-I" + str(WOLF)]
    sources = sorted((WOLF / "src").glob("*.c")) + sorted((WOLF / "wolfcrypt/src").glob("*.c"))

    def compile_wolf(pair):
        i, source = pair
        obj = work / f"wolf-{i}.o"
        command(os.environ.get("CC", "cc"), *flags, "-c", source, "-o", obj)
        return obj

    with ThreadPoolExecutor(max_workers=4) as pool:
        objects = list(pool.map(compile_wolf, enumerate(sources)))
    openssl = Path("/opt/homebrew/opt/openssl@3")
    crypto = ["-I" + str(openssl / "include"), "-L" + str(openssl / "lib")] if openssl.is_dir() else []
    binary = work / "protocol-host"
    command(os.environ.get("CXX", "c++"), "-std=c++20", "-Wno-deprecated-declarations", *flags,
            "-DREADER_SYNC_HOST_TEST=1", "-DFREEINK_NET_WOLFSSL=1", *crypto,
            "-I" + str(HERE / "host"), "-I" + str(SECURE / "include"), "-I" + str(work),
            "-I" + str(DEPS / "ArduinoJson/src"), "-I" + str(ROOT / "src/network"),
            HERE / "protocol_host.cpp", ROOT / "src/network/ReaderSyncProtocol.cpp",
            ROOT / "src/network/ReaderSyncStorage.cpp", ROOT / "src/network/ReaderSyncCanonical.cpp",
            SECURE / "src/SecureClient.cpp", *objects, "-lcrypto", "-o", binary)
    ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    ca = certificate(ca_key, ca_key, None, "Protocol regression CA", ca=True)
    ca_path = work / "root.pem"
    ca_path.write_bytes(ca.public_bytes(serialization.Encoding.PEM))
    leaf_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    leaf = certificate(leaf_key, ca_key, ca.subject, "localhost")
    leaf_path, key_path = work / "leaf.pem", work / "leaf.key"
    leaf_path.write_bytes(leaf.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(leaf_key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                              serialization.NoEncryption()))
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.load_cert_chain(leaf_path, key_path)
    state = None

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def reply(self, status, body, headers=None):
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            for key, value in (headers or {}).items():
                self.send_header(key, value)
            self.end_headers()
            self.wfile.write(body)

        def dispatch(self):
            body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            try:
                nonce = self.headers["X-Reader-Nonce"]
                stamp = self.headers["X-Reader-Timestamp"]
                assert self.headers["Authorization"] == "Bearer " + state.key
                assert self.headers["User-Agent"] == "ReaderSend/1"
                assert nonce and len(nonce) == 32 and nonce not in state.nonces
                assert abs(int(stamp) - time.time()) < 300
                canonical = "\n".join(["reader-sync-v1", self.command, self.path, hashlib.sha256(body).hexdigest(), stamp, nonce])
                expected = base64.urlsafe_b64encode(hmac.digest(state.key.encode(), canonical.encode(), "sha256")).decode().rstrip("=")
                assert hmac.compare_digest(self.headers["X-Reader-Signature"], expected)
                state.nonces.add(nonce)
                state.paths.append(self.path)
                if state.mode == "auth":
                    return self.reply(401, b'{"error":"unauthorized"}')
                if state.mode == "rate":
                    return self.reply(429, b'{"error":"limited"}', {"Retry-After": "125"})
                route = urlsplit(self.path)
                if self.command == "GET" and route.path == "/reader/items":
                    query = parse_qs(route.query)
                    assert query.get("limit") == ["8"] and set(query) <= {"limit", "after"}
                    after = query.get("after", [None])[0]
                    if after and not any(x["id"] == after for x in state.items):
                        return self.reply(400, b'{"error":"cursor vanished"}')
                    ready = [x for x in state.items if x["id"] not in state.acks and (not after or x["id"] > after)]
                    page = ready[:8]
                    more = len(ready) > 8
                    return self.reply(200, json.dumps(dict(items=page, has_more=more, next_after=page[-1]["id"] if more else None)).encode())
                if self.command == "GET" and route.path.endswith("/content"):
                    ident = route.path.split("/")[3]
                    state.gets.append(ident)
                    item = next(x for x in state.items if x["id"] == ident)
                    data = state.payload
                    if state.mode == "wronghash":
                        data = data[:-1] + bytes([data[-1] ^ 1])
                    elif state.mode == "short":
                        data = data[:-19]
                    elif state.mode == "overlong":
                        data += b"extra"
                    self.send_response(200)
                    self.send_header("Content-Type", "application/epub+zip")
                    self.send_header("X-Reader-Sha256", item["sha256"])
                    if state.mode in ("close", "short"):
                        self.send_header("Connection", "close")
                        self.end_headers()
                        self.wfile.write(data)
                        self.close_connection = True
                    else:
                        self.send_header("Transfer-Encoding", "chunked")
                        self.end_headers()
                        for start in range(0, len(data), 8191):
                            chunk = data[start:start + 8191]
                            self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
                        self.wfile.write(b"0\r\n\r\n")
                    return
                if self.command == "POST" and route.path.endswith("/ack"):
                    ident = route.path.split("/")[3]
                    ledger = json.loads((state.root / f".crosspoint/reader-sync/items/{ident}.json").read_text())
                    assert ledger["state"] == "complete"
                    installed = state.root / ledger["path"].lstrip("/")
                    assert installed.read_bytes() == state.payload
                    assert (state.root / ".crosspoint/library.dirty").is_file()
                    assert not body
                    state.acks.append(ident)
                    return self.reply(200, b'{"item":{"state":"consumed"}}')
                if self.command == "POST" and route.path == "/reader/annotations":
                    records = json.loads(body)["annotations"]
                    assert len(records) == 1 and records[0] == state.expected_note
                    state.posts.extend(records)
                    return self.reply(409 if state.mode == "noteconflict" else 201,
                                      b'{"error":"conflict"}' if state.mode == "noteconflict" else b'{"accepted":1,"inserted":1}')
                raise AssertionError("Unexpected route " + self.path)
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                return
            except Exception as exc:
                state.errors.append(repr(exc))
                self.reply(500, b'{"error":"fixture assertion"}')

        do_GET = do_POST = dispatch

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f"https://localhost:{server.server_port}"
    cases = 0

    def case(name, *, mode="chunked", payload=None, items=1, rounds=1, env=None, setup=None):
        global state, cases
        root = work / name
        (root / ".crosspoint").mkdir(parents=True)
        key = secrets.token_hex(32)
        config = dict(version=1, origin="https://robertzhou.dev", device_key=key,
                      device_id="6A4B4C73-B747-33DA-BF3D-455A3C03D0CB", enabled=True, interval_seconds=900)
        (root / ".crosspoint/reader-sync.json").write_text(json.dumps(config))
        state = State(root, key, payload or epub(), mode, items)
        if setup:
            setup(state, config)
        result = command(binary, root, origin, ca_path, rounds, env=dict(os.environ, **(env or {})), capture_output=True, text=True)
        rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
        assert len(rows) == rounds, result.stdout + result.stderr
        assert not state.errors, state.errors
        cases += 1
        return state, rows

    def no_install(s):
        assert not s.acks
        assert not list((s.root / "books/Inbox").glob("*.epub"))
        assert not list((s.root / ".crosspoint/reader-sync").glob("*.part"))

    try:
        s, rows = case("chunked-8MiB", payload=epub(8 * 1024 * 1024 - 1024))
        assert rows[0]["phase"] == 5 and len(s.acks) == 1 and rows[0]["books"] == 1
        # The sync log records transferred bytes: every response byte is counted as received, and
        # sent counts request bodies only (this run sends none: GETs and an empty-body ACK).
        assert rows[0]["rx"] >= len(s.payload) and rows[0]["tx"] == 0, rows[0]
        s, rows = case("close-delimited", mode="close")
        assert rows[0]["phase"] == 5 and len(s.acks) == 1
        for mode in ("wronghash", "short", "overlong"):
            s, rows = case(mode, mode=mode)
            no_install(s)
            assert rows[0]["phase"] != 5
        for name, env in (("full", {"READER_TEST_FREE_BYTES": "0"}),
                          ("write-failed", {"READER_TEST_WRITE_FAIL": "1"}),
                          ("rename-failed", {"READER_TEST_RENAME_FAIL": "1"}),
                          ("cancelled", {"READER_TEST_CANCEL": "1"})):
            s, rows = case(name, env=env, payload=epub(262144))
            no_install(s)
            assert rows[0]["phase"] != 5

        def collision(s, config):
            target = s.root / "books/Inbox/article-0.epub"
            target.parent.mkdir(parents=True)
            target.write_bytes(b"user's existing book")
        s, rows = case("collision", setup=collision, rounds=2)
        assert (s.root / "books/Inbox/article-0.epub").read_bytes() == b"user's existing book"
        assert rows[0]["books"] == 1 and rows[1]["books"] == 0 and len(s.gets) == 1
        assert (s.root / f"books/Inbox/article-0-{s.items[0]['sha256'][:8]}.epub").read_bytes() == s.payload

        def exhausted(s, config):
            collision(s, config)
            (s.root / f"books/Inbox/article-0-{s.items[0]['sha256'][:8]}.epub").write_bytes(b"second user book")
            (s.root / f"books/Inbox/{s.items[0]['id']}.epub").write_bytes(b"third user book")
        s, rows = case("exhausted", setup=exhausted)
        assert not s.acks and not s.gets and len(list((s.root / "books/Inbox").glob("*.epub"))) == 3

        def recovered(s, config):
            target = s.root / "books/Inbox/article-0.epub"
            target.parent.mkdir(parents=True)
            target.write_bytes(s.payload)
        s, rows = case("interrupted-after-rename", setup=recovered)
        assert len(s.acks) == 1 and not s.gets and rows[0]["books"] == 0
        s, rows = case("pagination", items=10, rounds=2)
        assert rows[0]["more"] and rows[0]["after"] and not rows[1]["more"] and len(s.acks) == 10
        assert any("&after=" in p for p in s.paths)
        s, rows = case("vanished-cursor", env={"READER_TEST_AFTER": str(uuid.uuid4())})
        assert len(s.acks) == 1 and sum(p.startswith("/reader/items?") for p in s.paths) == 2

        def note(s, config):
            native = dict(version=1, kind=0, id=1, sourcePath="/books/Inbox/article-0.epub", sourceTitle="中文 🌙",
                          chapterTitle="Title", chapterHref="chapter.xhtml", xpath="/body/p[1]", percentage=0.0000001,
                          si=0, pc=12, pp=1, excerpt="Quoted\ntext", note="A note", pageContext=False, createdAt=int(time.time()))
            folder = s.root / ".crosspoint/annotations/0123456789abcdef"
            folder.mkdir(parents=True)
            (folder / "001.json").write_text(json.dumps(native, ensure_ascii=False, separators=(",", ":")))
            s.expected_note = map_annotation(native, device_id=config["device_id"])
        s, rows = case("annotation-receipt", setup=note, rounds=2)
        assert len(s.posts) == 1 and rows[0]["notes"] == 1 and rows[1]["notes"] == 0
        assert rows[0]["tx"] > 0 and rows[0]["rx"] > 0, rows[0]
        s, rows = case("annotation-conflict", mode="noteconflict", setup=note, rounds=2)
        assert len(s.posts) == 1 and rows[0]["rejected"] == 1 and rows[1]["rejected"] == 1
        def paged_notes(s, config):
            note(s, config)
            for book in range(2):
                folder = s.root / f".crosspoint/annotations/{book:016x}"
                folder.mkdir()
                for ident in range(128):
                    (folder / f"{ident:03d}.json").write_text("{}")
        s, rows = case("annotation-scan-cursor", setup=paged_notes, rounds=2)
        assert rows[0]["more"] and not rows[1]["more"] and len(s.posts) == 1
        assert sum(row["rejected"] for row in rows) == 256
        def vanished_note_cursor(s, config):
            note(s, config)
            folder = s.root / ".crosspoint/reader-sync"
            folder.mkdir()
            (folder / "note-cursor").write_text("ffffffffffffffff/999.json")
        s, rows = case("annotation-vanished-cursor", setup=vanished_note_cursor)
        assert len(s.posts) == 1 and rows[0]["notes"] == 1
        s, rows = case("auth", mode="auth")
        assert rows[0]["phase"] == 12 and not s.acks
        s, rows = case("rate", mode="rate")
        assert rows[0]["phase"] == 13 and rows[0]["retry"] == 125 and not s.acks

        # --- schedule: backward-compatible config parsing -------------------------------------
        legacy_keys = {"version", "origin", "device_key", "device_id", "enabled", "interval_seconds"}

        def config_with(**extra):
            def setup(s, config):
                (s.root / ".crosspoint/reader-sync.json").write_text(json.dumps(dict(config, **extra)))
            return setup

        def on_disk(s):
            return json.loads((s.root / ".crosspoint/reader-sync.json").read_text())

        # An existing six-field file (interval_seconds=900) still loads, defaults to events and is
        # written back without a schedule key, so firmware from before this change still accepts it.
        s, rows = case("schedule-legacy-900")
        assert rows[0]["schedule"] == "events" and rows[0]["explicit"] is False and rows[0]["interval"] == 900, rows[0]
        assert set(on_disk(s)) == legacy_keys and on_disk(s)["interval_seconds"] == 900
        s, rows = case("schedule-legacy-3600", setup=config_with(interval_seconds=3600))
        assert rows[0]["schedule"] == "events" and rows[0]["explicit"] is False and rows[0]["interval"] == 3600, rows[0]
        assert set(on_disk(s)) == legacy_keys
        # An explicit schedule loads, round-trips and keeps interval_seconds meaningful.
        s, rows = case("schedule-explicit-interval", setup=config_with(schedule="interval"))
        assert rows[0]["schedule"] == "interval" and rows[0]["explicit"] is True and rows[0]["interval"] == 900, rows[0]
        assert set(on_disk(s)) == legacy_keys | {"schedule"} and on_disk(s)["schedule"] == "interval"
        s, rows = case("schedule-explicit-events", setup=config_with(schedule="events", interval_seconds=3600))
        assert rows[0]["schedule"] == "events" and rows[0]["explicit"] is True and rows[0]["interval"] == 3600, rows[0]
        assert on_disk(s)["schedule"] == "events"
        # Anything else is rejected rather than silently falling back to a schedule the user did not choose.
        for name, extra in (("unknown-word", dict(schedule="hourly")), ("wrong-case", dict(schedule="Events")),
                            ("empty", dict(schedule="")), ("null", dict(schedule=None)),
                            ("number", dict(schedule=1)), ("bool", dict(schedule=True)),
                            ("unknown-key", dict(sync_schedule="events"))):
            s, rows = case("schedule-invalid-" + name, setup=config_with(**extra))
            assert rows[0] == {"invalid_config": True}, (name, rows[0])
        print(f"PASS: {cases} real HTTPS protocol/storage scenarios; signatures, streamed bytes, durable ACKs and canonical notes")
    finally:
        server.shutdown()
        server.server_close()
