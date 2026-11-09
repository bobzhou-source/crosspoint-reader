import base64
import hashlib
import hmac
import io
import json
import os
import tempfile
import unittest
import zipfile
from pathlib import Path
from urllib.parse import parse_qs
from unittest.mock import patch

from reader_send import card, card_autosync, cli
from reader_send.cache import Cache
from reader_send.card import BOOK_INBOX, CardVolume
from reader_send.cloud import CloudClient, Response, json_bytes
from reader_send.cloud_sync import pull_ready
from reader_send.common import ReaderError
from reader_send.model import ReadingItem

KEY = "0123456789abcdef" * 4
NOW = 1793318400
UUID = "00000000-0000-0000-0000-000000000000"
OTHER_UUID = "11111111-2222-3333-4444-555555555555"
FIRST = "aaaaaaaa-bbbb-cccc-dddd-000000000001"
SECOND = "aaaaaaaa-bbbb-cccc-dddd-000000000002"


def epub_bytes(text="one"):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as archive:
        archive.writestr(zipfile.ZipInfo("mimetype", (2026, 1, 1, 0, 0, 0)), b"application/epub+zip")
        archive.writestr(zipfile.ZipInfo("content.xhtml", (2026, 1, 1, 0, 0, 0)), f"<p>{text}</p>".encode())
    return stream.getvalue()


class FakeServer:
    """The reader API's inbox routes (list, content, ack) with switchable HTTP 429 answers."""

    def __init__(self):
        self.items = {}
        self.blobs = {}
        self.calls = []
        self.limit = set()  # (method, route-prefix) pairs answered 429
        self.window_limit = None  # requests per simulated minute before answering 429; None = unlimited
        self.window_count = 0
        self.windows = []  # request count of every finished simulated minute
        self.throttled = []  # requests answered 429 because the window was full

    def add(self, item_id, data, title="Remote book"):
        self.items[item_id] = {"id": item_id, "title": title, "original_url": None,
                               "source_path": f"/books/{item_id}.epub", "sha256": hashlib.sha256(data).hexdigest(),
                               "size": len(data), "state": "ready", "created_at": NOW * 1000, "consumed_at": None}
        self.blobs[item_id] = data
        return data

    def __call__(self, origin, method, route, body, headers, timeout, max_bytes):
        self.calls.append((method, route))
        self.window_count += 1
        if self.window_limit is not None and self.window_count > self.window_limit:
            self.throttled.append((method, route))
            return Response(429, {}, b"")
        assert headers["Authorization"] == "Bearer " + KEY
        canonical = "\n".join(["reader-sync-v1", method, route, hashlib.sha256(body).hexdigest(),
                               headers["X-Reader-Timestamp"], headers["X-Reader-Nonce"]])
        expected = base64.urlsafe_b64encode(
            hmac.new(KEY.encode(), canonical.encode(), hashlib.sha256).digest()).decode().rstrip("=")
        assert headers["X-Reader-Signature"] == expected
        for limited_method, prefix in self.limit:
            if method == limited_method and route.startswith(prefix):
                return Response(429, {}, b"")
        path = route.partition("?")[0]
        if path == "/reader/items" and method == "GET":
            query = parse_qs(route.partition("?")[2])
            active = [dict(item) for item in self.items.values() if item["state"] != "consumed"]
            start = 0
            if "after" in query:
                start = [item["id"] for item in active].index(query["after"][0]) + 1
            page = active[start:start + 64]
            more = start + 64 < len(active)
            return Response(200, {}, json_bytes({"items": page, "has_more": more,
                                                 "next_after": page[-1]["id"] if more else None}))
        item_id = path.split("/")[3]
        if path.endswith("/ack") and method == "POST":
            self.items[item_id]["state"] = "consumed"
            self.items[item_id]["consumed_at"] = NOW * 1000
            self.blobs.pop(item_id, None)
            return Response(200, {}, json_bytes({"item": self.items[item_id]}))
        if path.endswith("/content") and method == "GET":
            data = self.blobs[item_id]
            return Response(200, {"x-reader-sha256": self.items[item_id]["sha256"],
                                  "content-length": str(len(data))}, data)
        raise AssertionError(f"unexpected request {method} {route}")

    def routes(self, method, suffix=""):
        return [route for call_method, route in self.calls if call_method == method and route.endswith(suffix)]

    def acked(self):
        return sorted(item_id for item_id, item in self.items.items() if item["state"] == "consumed")

    def pulled(self):
        return sorted(route.split("/")[3] for route in self.routes("GET", "/content"))

    def new_minute(self):
        self.windows.append(self.window_count)
        self.window_count = 0


class CardAutosyncTest(unittest.TestCase):
    def setUp(self):
        environment = patch.dict("os.environ", {}, clear=False)
        environment.start()
        self.addCleanup(environment.stop)
        os.environ.pop("READER_SYNC_KEY", None)
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.card = self.root / "card"
        self.card.mkdir()
        self.cache = Cache(self.root / "cache")
        self.server = FakeServer()
        self.counter = 0
        self.notes: list[str] = []
        self.config = {"version": 1, "volume_uuid": UUID, "annotations_dir": str(self.root / "annotations"),
                       "firmware_path": str(self.root / "fw.bin"), "firmware_sha256": "0" * 64,
                       "launchers_dir": str(self.root / "launchers")}
        self.log_path = self.root / "autosync.log"
        self.key_reads = 0

    def nonce(self):
        self.counter += 1
        return f"{self.counter:032x}"

    def client(self):
        return CloudClient(transport=self.server, clock=lambda: NOW, nonce=self.nonce)

    def key(self):
        self.key_reads += 1
        return KEY, ""

    def run_autosync(self, volume_uuid=UUID, resolve=None, wait=lambda seconds: None, **options):
        resolve = resolve or (lambda uuid: CardVolume(self.card, "disk9s1", volume_uuid))
        return card_autosync.autosync(
            self.cache, self.config, resolve=resolve, client_factory=self.client, key_provider=self.key,
            notify=self.notes.append, log=card_autosync.RunLog(self.log_path), wait=wait, **options)

    def inbox(self):
        directory = self.card / BOOK_INBOX
        return sorted(path.name for path in directory.iterdir()) if directory.is_dir() else []

    def inbox_bytes(self):
        directory = self.card / BOOK_INBOX
        return sorted(path.read_bytes() for path in directory.iterdir()) if directory.is_dir() else []

    def tree(self):
        return sorted(str(path.relative_to(self.card)) for path in self.card.rglob("*"))

    def cache_unrelated_book(self):
        item = ReadingItem(title="Old cached article", author="", source_url="https://example.com/old",
                           date="", language="", content="", images=[])
        return self.cache.store("c" * 24, item, epub_bytes("unrelated cache entry"))

    def test_delivers_only_cloud_books_verifies_then_acks(self):
        first = self.server.add(FIRST, epub_bytes("first"), "First book")
        second = self.server.add(SECOND, epub_bytes("second"), "Second book")
        self.cache_unrelated_book()
        novels = self.card / "books/Novels"
        shelf = self.card / "books/Books"
        novels.mkdir(parents=True)
        shelf.mkdir(parents=True)
        (novels / "keep.epub").write_bytes(b"novel")
        (shelf / "keep.epub").write_bytes(b"book")

        result = self.run_autosync()

        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["delivered"], 2)
        self.assertEqual(sorted(result["acked"]), [FIRST, SECOND])
        self.assertEqual(self.inbox_bytes(), sorted([first, second]))  # byte-identical; unrelated cache entry stays home
        self.assertEqual(self.server.acked(), [FIRST, SECOND])
        self.assertEqual((novels / "keep.epub").read_bytes(), b"novel")
        self.assertEqual((shelf / "keep.epub").read_bytes(), b"book")
        self.assertEqual(sorted(path.name for path in novels.iterdir()), ["keep.epub"])
        self.assertEqual(sorted(path.name for path in shelf.iterdir()), ["keep.epub"])
        self.assertTrue((self.card / ".crosspoint/library.dirty").exists())
        self.assertEqual(self.notes, ["2 books delivered to the reader card"])
        log = self.log_path.read_text(encoding="utf-8")
        self.assertIn("delivered and acknowledged: " + FIRST, log)
        self.assertNotIn(KEY, log)
        self.assertNotIn(KEY, json.dumps(result))
        self.assertNotIn("READER_SYNC_KEY", os.environ)

    def test_ack_happens_only_after_the_card_copy_matches_the_checksum(self):
        good = self.server.add(FIRST, epub_bytes("good"))
        self.server.add(SECOND, epub_bytes("bad"))
        original = card._copy_atomic
        outcome = {}

        def flaky_copy(source, destination):
            original(source, destination)
            if hashlib.sha256(source.read_bytes()).hexdigest() == self.server.items[SECOND]["sha256"]:
                destination.write_bytes(b"truncated")  # the card stored wrong bytes
            outcome.setdefault("acked_at_copy", list(self.server.acked()))

        with patch("reader_send.card._copy_atomic", flaky_copy):
            result = self.run_autosync()

        self.assertEqual(result["acked"], [FIRST])
        self.assertEqual(result["unverified"], [{"id": SECOND}])
        self.assertEqual(self.server.acked(), [FIRST])
        self.assertEqual(self.server.items[SECOND]["state"], "ready")
        self.assertEqual(outcome["acked_at_copy"], [])  # nothing was acknowledged before its copy existed
        self.assertIn(good, self.inbox_bytes())
        self.assertEqual(self.notes, ["1 book delivered to the reader card; 1 held back (copy not verified)"])
        self.assertIn("not acknowledged (card copy did not verify): " + SECOND,
                      self.log_path.read_text(encoding="utf-8"))

    def test_rerun_is_idempotent_and_leaves_the_card_alone(self):
        self.server.add(FIRST, epub_bytes("first"))
        self.run_autosync()
        before = (self.tree(), self.inbox_bytes(), self.server.acked())
        calls = len(self.server.calls)

        again = self.run_autosync()

        self.assertEqual(again["status"], "ok")
        self.assertEqual(again["delivered"], 0)
        self.assertEqual((self.tree(), self.inbox_bytes(), self.server.acked()), before)
        self.assertEqual([method for method, _ in self.server.calls[calls:]], ["GET"])  # one listing, nothing else
        self.assertEqual(self.notes[-1], "0 books delivered to the reader card")

    def test_wrong_volume_or_absent_card_does_nothing(self):
        self.server.add(FIRST, epub_bytes("first"))

        wrong = self.run_autosync(volume_uuid=OTHER_UUID)

        def missing(uuid):
            raise ReaderError("Reader card is not mounted")

        absent = self.run_autosync(resolve=missing)

        self.assertEqual(wrong["status"], "skipped")
        self.assertEqual(absent["status"], "skipped")
        self.assertEqual(self.server.calls, [])
        self.assertEqual(self.key_reads, 0)
        self.assertEqual(self.tree(), [])
        self.assertEqual(self.notes, [])
        self.assertFalse((self.root / "annotations").exists())
        self.assertFalse((self.root / "cache").exists())

    def test_rate_limit_on_listing_stops_without_acks_and_next_run_recovers(self):
        data = self.server.add(FIRST, epub_bytes("first"))
        self.server.limit = {("GET", "/reader/items")}

        limited = self.run_autosync()

        self.assertEqual(limited["status"], "rate_limited")
        self.assertEqual(self.server.acked(), [])
        self.assertEqual(self.inbox(), [])
        self.assertIn("HTTP 429", self.log_path.read_text(encoding="utf-8"))
        self.assertIn("rate limit", self.notes[-1])
        self.assertEqual(len(self.server.calls), 1)  # no retry storm

        self.server.limit = set()
        recovered = self.run_autosync()

        self.assertEqual(recovered["status"], "ok")
        self.assertEqual(self.inbox_bytes(), [data])
        self.assertEqual(self.server.acked(), [FIRST])

    def test_rate_limit_during_pull_acks_nothing_and_resumes_without_duplicates(self):
        self.server.add(FIRST, epub_bytes("first"))
        self.server.add(SECOND, epub_bytes("second"))
        self.server.limit = {("GET", f"/reader/items/{SECOND}/content")}

        limited = self.run_autosync()

        self.assertEqual(limited["status"], "rate_limited")
        self.assertEqual(self.server.acked(), [])
        self.assertEqual(self.inbox(), [])

        self.server.limit = set()
        recovered = self.run_autosync()

        self.assertEqual(recovered["status"], "ok")
        self.assertEqual(self.server.acked(), [FIRST, SECOND])
        self.assertEqual(len(self.inbox()), 2)
        self.assertEqual(self.server.pulled().count(FIRST), 1)  # the first book was not downloaded twice

    def test_rate_limit_while_acking_keeps_the_acks_that_succeeded_and_retries_the_rest(self):
        self.server.add(FIRST, epub_bytes("first"))
        self.server.add(SECOND, epub_bytes("second"))
        self.server.limit = {("POST", f"/reader/items/{SECOND}/ack")}

        limited = self.run_autosync()

        self.assertEqual(limited["status"], "rate_limited")
        self.assertEqual(limited["acked"], [FIRST])
        self.assertEqual(len(self.inbox()), 2)

        self.server.limit = set()
        pulls = len(self.server.pulled())
        recovered = self.run_autosync()

        self.assertEqual(recovered["status"], "ok")
        self.assertEqual(self.server.acked(), [FIRST, SECOND])
        self.assertEqual(len(self.server.pulled()), pulls)  # re-verified from the cache, not re-downloaded
        self.assertEqual(len(self.inbox()), 2)

    def test_annotations_are_imported_even_when_the_cloud_is_unreachable(self):
        source = self.card / ".crosspoint/annotations/0000000000000001"
        source.mkdir(parents=True)
        (source / "001.json").write_text(json.dumps({
            "version": 1, "id": 1, "kind": 0, "sourcePath": "/books/Example.epub", "sourceTitle": "Example",
            "chapterTitle": "One", "chapterHref": "one.xhtml", "xpath": "/body/p[1]", "percentage": 0.25,
            "si": 0, "pc": 0, "pp": 0, "excerpt": "Exact excerpt", "note": "A note", "pageContext": False,
            "createdAt": 1700000000}), encoding="utf-8")

        result = card_autosync.autosync(
            self.cache, self.config, resolve=lambda uuid: CardVolume(self.card, "disk9s1", UUID),
            key_provider=lambda: ("", "No Keychain item"), notify=self.notes.append,
            log=card_autosync.RunLog(self.log_path))

        self.assertEqual(result["status"], "error")
        self.assertEqual(result["annotations"]["records"], 1)
        imported = Path(self.config["annotations_dir"]) / f"reading-inbox-{UUID.lower()}.md"
        self.assertIn("Exact excerpt", imported.read_text(encoding="utf-8"))
        self.assertEqual(self.inbox(), [])
        self.assertIn("1 annotation imported", self.notes[-1])
        self.assertNotIn("READER_SYNC_KEY", os.environ)

    def test_shelves_are_copied_and_marked_dirty_even_when_the_cloud_is_unreachable(self):
        shelf = self.root / "shelf"
        shelf.mkdir()
        (shelf / "Local Book.epub").write_bytes(epub_bytes("local"))

        result = card_autosync.autosync(
            self.cache, self.config, resolve=lambda uuid: CardVolume(self.card, "disk9s1", UUID),
            key_provider=lambda: ("", "No Keychain item"), notify=self.notes.append,
            log=card_autosync.RunLog(self.log_path), books_dir=shelf)

        self.assertEqual(result["status"], "error")  # the missing key is still reported
        self.assertEqual(result["shelves"]["books"]["copied"], 1)
        self.assertIsNone(result["shelves"]["novels"])
        self.assertEqual((self.card / "books/Books/Local Book.epub").read_bytes(), epub_bytes("local"))
        self.assertTrue((self.card / card.LIBRARY_DIRTY).is_file())
        log = self.log_path.read_text(encoding="utf-8")
        self.assertIn("books shelf: copied 1, unchanged 0, errors 0", log)

    def test_models_and_the_story_graph_are_synced_and_logged(self):
        shelf, models = self.root / "shelf", self.root / "models"
        shelf.mkdir()
        models.mkdir()
        (shelf / "Local Book.epub").write_bytes(epub_bytes("local"))
        (models / "cspw.bin").write_bytes(b"CSPW" + bytes(508))
        (models / "pe-writer.bin").write_bytes(b"PEWR" + (1).to_bytes(2, "little") + (128).to_bytes(2, "little") + bytes(56))
        (models / "writer-clauses.bin").write_bytes(b"WCLS" + (1).to_bytes(2, "little") + (64).to_bytes(2, "little") + bytes(1_048_600))

        result = self.run_autosync(books_dir=shelf, models_dir=models)

        shelf_result = result["shelves"]["models"]
        self.assertEqual(shelf_result["errors"], [])
        self.assertEqual(shelf_result["missing"], ["platonic-hnet.bin", "engram.bin"])
        self.assertEqual(sorted(path.name for path in (self.card / "models").iterdir()),
                         ["cspw.bin", "inbox-concepts.idx", "pe-writer.bin", "story-graph.idx",
                          "writer-clauses.bin"])
        self.assertEqual((self.card / "models/writer-clauses.bin").read_bytes()[:4], b"WCLS")
        self.assertEqual((self.card / "models/story-graph.idx").read_bytes()[:4], b"SGRF")
        log = self.log_path.read_text(encoding="utf-8")
        self.assertIn("models shelf: copied 5, unchanged 0, errors 0", log)
        self.assertIn("story graph: 0 entities from 1 books, extracted 0, unreadable 1, pending 0", log)
        self.assertIn("models missing from the Mac folder: platonic-hnet.bin, engram.bin", log)

    def test_shelf_errors_make_the_run_fail_without_losing_the_cloud_books(self):
        self.server.add(FIRST, epub_bytes("first"))
        shelf = self.root / "shelf"
        shelf.mkdir()
        outside = self.root / "outside.epub"
        outside.write_bytes(b"outside")
        os.symlink(outside, shelf / "linked.epub")

        result = self.run_autosync(books_dir=shelf)

        self.assertEqual(result["status"], "error")
        self.assertEqual(result["delivered"], 1)
        self.assertEqual(result["acked"], [FIRST])
        self.assertEqual(result["shelves"]["books"]["copied"], 0)
        self.assertTrue(any("symlink" in error for error in result["errors"]))

    def test_a_second_run_while_one_is_active_exits_without_touching_anything(self):
        self.server.add(FIRST, epub_bytes("first"))
        held = card_autosync._acquire_lock(self.cache)
        self.addCleanup(held.close)

        result = self.run_autosync()

        self.assertEqual(result["status"], "busy")
        self.assertEqual(self.server.calls, [])
        self.assertEqual(self.tree(), [])

    def test_backlog_beyond_one_batch_continues_in_later_passes(self):
        ids = [f"aaaaaaaa-bbbb-cccc-dddd-{number:012d}" for number in range(1, 4)]
        for item_id in ids:
            self.server.add(item_id, epub_bytes(item_id))
        waits = []

        result = card_autosync.autosync(
            self.cache, self.config, resolve=lambda uuid: CardVolume(self.card, "disk9s1", UUID),
            client_factory=self.client, key_provider=self.key, notify=None, batch=2, passes=3,
            wait=waits.append)

        self.assertEqual(sorted(result["acked"]), ids)
        self.assertEqual(result["passes"], 2)
        self.assertEqual(waits, [card_autosync.PASS_WAIT_SECONDS])

    def test_command_line_runs_end_to_end_with_the_key_from_the_environment(self):
        self.server.add(FIRST, epub_bytes("first"))
        config_path = self.root / "card.json"
        config_path.write_text(json.dumps({
            "version": 1, "volume_uuid": UUID, "annotations_dir": self.config["annotations_dir"],
            "firmware_path": self.config["firmware_path"], "firmware_sha256": "0" * 64,
            "launchers_dir": self.config["launchers_dir"]}), encoding="utf-8")
        os.environ["READER_SYNC_KEY"] = KEY
        real_client = CloudClient

        def client(origin, timeout):
            return real_client(origin, timeout, transport=self.server, clock=lambda: NOW, nonce=self.nonce)

        arguments = ["--cache-dir", str(self.cache.root), "card", "autosync", "--config", str(config_path),
                     "--log", str(self.log_path), "--no-notify"]
        with patch.object(card, "resolve_card", lambda uuid: CardVolume(self.card, "disk9s1", UUID)), \
                patch.object(card_autosync, "CloudClient", client), \
                patch.object(card, "default_shelves", dict), \
                patch("sys.stdout", new_callable=io.StringIO) as stdout:
            code = cli.main(arguments)

        self.assertEqual(code, 0)
        self.assertEqual(json.loads(stdout.getvalue())["acked"], [FIRST])
        self.assertEqual(self.server.acked(), [FIRST])
        self.assertEqual(os.environ["READER_SYNC_KEY"], KEY)  # restored, not leaked or removed
        with patch.object(card, "resolve_card", lambda uuid: CardVolume(self.card, "disk9s1", OTHER_UUID)), \
                patch("sys.stdout", new_callable=io.StringIO):
            self.assertEqual(cli.main(arguments), 0)

    def precache(self, ids):
        """Put already-listed server books into the cache, then forget the requests that took."""
        with patch.dict(os.environ, {"READER_SYNC_KEY": KEY}):
            client = self.client()
            wanted = [item for item in client.items(ready_only=True) if item["id"] in set(ids)]
            pull_ready(client, self.cache, wanted, limit=len(wanted))
        self.server.calls.clear()
        self.server.windows.clear()
        self.server.window_count = 0

    def test_field_shaped_backlog_stays_inside_the_request_allowance_and_delivers_the_most_per_mount(self):
        ids = [f"aaaaaaaa-bbbb-cccc-dddd-{number:012d}" for number in range(1, 257)]
        data = {item_id: self.server.add(item_id, epub_bytes(item_id)) for item_id in ids}
        self.precache(ids[:49])  # the 11:35 field run: 256 ready, 49 already cached
        self.server.window_limit = card_autosync.REQUEST_ALLOWANCE

        result = self.run_autosync(wait=lambda seconds: self.server.new_minute())
        self.server.new_minute()

        self.assertEqual(self.server.throttled, [])  # no request was answered 429
        self.assertEqual(result["status"], "ok")
        # Pass 1: 4 listing pages leave 52 requests: 49 cached acks and 1 download + ack. Passes 2 and 3: 26 downloads + 26 acks.
        self.assertEqual(len(result["acked"]), 49 + 1 + 26 + 26)  # the unbudgeted run delivered 36 and hit a 429
        self.assertEqual(result["acked"][:49], ids[:49])  # books that only need an ack go first
        self.assertLessEqual(max(self.server.windows), card_autosync.REQUEST_ALLOWANCE - card_autosync.REQUEST_MARGIN)
        self.assertEqual(self.server.acked(), sorted(result["acked"]))
        self.assertEqual(self.inbox_bytes(), sorted(data[item_id] for item_id in result["acked"]))  # every ack has its verified card copy

    def test_cached_books_are_acknowledged_first_and_never_without_a_verified_card_copy(self):
        ids = [f"aaaaaaaa-bbbb-cccc-dddd-{number:012d}" for number in range(1, 5)]
        first, second, third, fourth = ids
        data = {item_id: self.server.add(item_id, epub_bytes(item_id)) for item_id in ids}
        self.precache([first, second])
        original = card._copy_atomic

        def flaky_copy(source, destination):
            original(source, destination)
            if source.read_bytes() == data[second]:
                destination.write_bytes(b"truncated")  # the card stored wrong bytes

        with patch("reader_send.card._copy_atomic", flaky_copy):
            result = self.run_autosync(passes=1)

        self.assertEqual(result["acked"], [first, third, fourth])
        self.assertEqual([route.split("/")[3] for route in self.server.routes("POST", "/ack")], [first, third, fourth])
        self.assertEqual(result["unverified"], [{"id": second}])
        self.assertEqual(self.server.items[second]["state"], "ready")

    def test_cached_acks_beyond_the_budget_carry_to_the_next_mount_without_downloads_or_throttling(self):
        ids = [f"aaaaaaaa-bbbb-cccc-dddd-{number:012d}" for number in range(1, 71)]
        for item_id in ids:
            self.server.add(item_id, epub_bytes(item_id))
        self.precache(ids)
        self.server.window_limit = card_autosync.REQUEST_ALLOWANCE

        first = self.run_autosync(passes=1)
        self.server.new_minute()
        second = self.run_autosync(passes=1)

        self.assertEqual(self.server.throttled, [])
        self.assertEqual((len(first["acked"]), len(second["acked"])), (54, 16))  # 2 listing pages + 4 margin leave 54 acks
        self.assertEqual(self.server.acked(), sorted(ids))
        self.assertEqual(self.server.routes("GET", "/content"), [])  # nothing was downloaded again

    def test_a_copy_verified_on_an_earlier_run_is_not_read_back_again(self):
        self.server.add(FIRST, epub_bytes("first"))
        self.server.add(SECOND, epub_bytes("second"))
        self.server.limit = {("POST", f"/reader/items/{SECOND}/ack")}
        self.assertEqual(self.run_autosync()["status"], "rate_limited")
        self.server.limit = set()

        with patch.object(card_autosync, "card_file_sha256", wraps=card_autosync.card_file_sha256) as hashed:
            again = self.run_autosync()

        self.assertEqual(again["acked"], [SECOND])
        hashed.assert_not_called()

    def test_verified_item_trusts_a_proof_only_while_size_and_mtime_match(self):
        data = epub_bytes("proof")
        sha = hashlib.sha256(data).hexdigest()
        path = self.card / BOOK_INBOX / "x.epub"
        path.parent.mkdir(parents=True)
        path.write_bytes(data)
        volume = CardVolume(self.card, "disk9s1", UUID)
        record, item = {"sha256": sha, "size": len(data)}, {"sha256": sha}
        entry = {"filename": "x.epub", "state": "complete", "verified": {"sha256": sha, "card": card._file_stamp(path)}}

        with patch.object(card_autosync, "card_file_sha256", wraps=card_autosync.card_file_sha256) as hashed:
            self.assertEqual(card_autosync._verified_item(volume, entry, item, record), (True, None))
            hashed.assert_not_called()

            # Same size, different bytes, new mtime: read back, and rejected.
            status = path.stat()
            path.write_bytes(data[:-1] + bytes([data[-1] ^ 0xFF]))
            os.utime(path, ns=(status.st_atime_ns, status.st_mtime_ns + 10_000_000_000))
            self.assertEqual(card_autosync._verified_item(volume, entry, item, record), (False, None))
            self.assertEqual(hashed.call_count, 1)

            # Good bytes again with yet another mtime: read back, accepted, and a fresh proof is returned.
            path.write_bytes(data)
            os.utime(path, ns=(status.st_atime_ns, status.st_mtime_ns + 20_000_000_000))
            verified, proof = card_autosync._verified_item(volume, entry, item, record)
            self.assertEqual((verified, hashed.call_count), (True, 2))
            self.assertEqual(proof, (sha, card._file_stamp(path)))

        # No ledger proof at all, a pending entry, or a proof for other bytes: always read back.
        for other in (None, {"filename": "x.epub", "state": "pending"},
                      {"filename": "x.epub", "state": "complete", "verified": {"sha256": "f" * 64, "card": card._file_stamp(path)}}):
            with patch.object(card_autosync, "card_file_sha256", wraps=card_autosync.card_file_sha256) as hashed:
                card_autosync._verified_item(volume, other, item, record)
            self.assertEqual(hashed.call_count, 0 if other is None or other["state"] == "pending" else 1)

    def test_card_file_hash_rejects_symlinks_and_missing_files(self):
        real = self.root / "real.bin"
        real.write_bytes(b"abc")
        link = self.root / "link.bin"
        link.symlink_to(real)

        self.assertEqual(card_autosync.card_file_sha256(real), hashlib.sha256(b"abc").hexdigest())
        self.assertIsNone(card_autosync.card_file_sha256(link))
        self.assertIsNone(card_autosync.card_file_sha256(self.root / "absent.bin"))


if __name__ == "__main__":
    unittest.main()
