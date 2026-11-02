import hashlib
import hmac
import base64
import io
import json
import tempfile
import unittest
import uuid
import zipfile
from pathlib import Path
from unittest.mock import patch

from reader_send.cache import Cache
from reader_send.cloud import CloudClient, CloudContentReleased, Response, json_bytes, prepare_push
from reader_send.cloud_drain import drain
from reader_send.cloud_sync import cache_id_for, sync
from reader_send.common import ReaderError
from reader_send.model import ReadingItem

KEY = "0123456789abcdef" * 4
NOW = 1793318400
ITEM_ID = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
OTHER_ID = "bbbbbbbb-bbbb-cccc-dddd-eeeeeeeeeeee"


def epub_bytes(padding=b""):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as archive:
        info = zipfile.ZipInfo("mimetype", (2026, 1, 1, 0, 0, 0))
        archive.writestr(info, b"application/epub+zip")
        info = zipfile.ZipInfo("content.xhtml", (2026, 1, 1, 0, 0, 0))
        archive.writestr(info, b"<p>Synced EPUB</p>" + padding)
    return stream.getvalue()


def item(data=None, **changes):
    data = epub_bytes() if data is None else data
    return {"id": ITEM_ID, "title": "Remote book", "original_url": None,
            "source_path": "/books/remote.epub", "sha256": hashlib.sha256(data).hexdigest(),
            "size": len(data), **changes}


def annotation(number=1, **changes):
    return {"id": str(uuid.UUID(int=number)), "item_id": None, "book_path": "/books/source.html",
            "source_anchor": '{"xpath":"/html/body/p[1]"}', "selection": "quoted", "kind": "quote",
            "note": "", "created_at": 0, **changes}


def native_annotation(**changes):
    return {
        "version": 1,
        "id": 3,
        "kind": 1,
        "sourcePath": "/books/card-source.epub",
        "sourceTitle": "Card source",
        "chapterTitle": "Chapter 2",
        "chapterHref": "chapter-2.xhtml",
        "xpath": "/html/body/p[4]",
        "percentage": 0.375,
        "si": 1,
        "pc": 12,
        "pp": 4,
        "vo": 0,
        "excerpt": " exact card selection ",
        "note": "return this note unchanged",
        "pageContext": False,
        "createdAt": NOW,
        "nativeExtra": {"preserve": "yes"},
        **changes,
    }


class FakeAPI:
    """Same wire shape as the real Pages Function, without DNS or TLS."""

    def __init__(self):
        self.calls = []
        self.items = {}
        self.blobs = {}
        self.annotations = {}
        self.nonces = set()
        self.after_list = None
        self.content_override = {}

    def __call__(self, origin, method, route, body, headers, timeout, max_bytes):
        self.calls.append((method, route, body))
        assert headers["Authorization"] == "Bearer " + KEY
        canonical = "\n".join(["reader-sync-v1", method, route, hashlib.sha256(body).hexdigest(),
                               headers["X-Reader-Timestamp"], headers["X-Reader-Nonce"]])
        expected = base64.urlsafe_b64encode(
            hmac.new(KEY.encode(), canonical.encode(), hashlib.sha256).digest()).decode().rstrip("=")
        assert headers["X-Reader-Signature"] == expected
        assert headers["User-Agent"] == "ReaderSend/1"
        assert headers["X-Reader-Nonce"] not in self.nonces
        self.nonces.add(headers["X-Reader-Nonce"])
        path, _, query = route.partition("?")
        if path == "/reader/items":
            if method == "GET":
                listing = [dict(value) for value in self.items.values()]
                if self.after_list is not None:
                    after_list, self.after_list = self.after_list, None
                    after_list()
                return self.response(200, {"items": listing, "has_more": False, "next_after": None})
            metadata = json.loads(body)
            prior = self.items.get(metadata["id"])
            if prior and any(prior[key] != value for key, value in metadata.items()):
                return self.response(409, {"error": "conflict"})
            if not prior:
                self.items[metadata["id"]] = {**metadata, "state": "uploading",
                                              "created_at": NOW * 1000, "consumed_at": None}
            return self.response(200 if prior else 201, {"item": self.items[metadata["id"]]})
        if path == "/reader/annotations" and method == "POST":
            values = json.loads(body)["annotations"]
            if any(value["id"] in self.annotations and self.annotations[value["id"]] != value
                   for value in values):
                return self.response(409, {"error": "conflict"})
            inserted = sum(value["id"] not in self.annotations for value in values)
            self.annotations.update({value["id"]: value for value in values})
            return self.response(201 if inserted else 200,
                                 {"accepted": len(values), "inserted": inserted})
        if path == "/reader/annotations/export":
            after = int(query.split("after=")[1].split("&")[0])
            limit = int(query.split("limit=")[1])
            records = [record for record in self.ordered_annotations() if record["sequence"] > after][:limit]
            payload = b"".join(json.dumps(record).encode() + b"\n" for record in records)
            return Response(200, {}, payload)
        if path == "/reader/annotations":
            after, limit = 0, 100
            for part in query.split("&"):
                key, _, value = part.partition("=")
                if key == "after":
                    after = int(value)
                elif key == "limit":
                    limit = int(value)
            records = [record for record in self.ordered_annotations() if record["sequence"] > after][:limit]
            return self.response(200, {"annotations": records, "next_after": None})
        item_id = path.split("/")[3]
        if path.endswith("/ack"):
            return self.response(409, {"error": "unused"})
        if method == "DELETE":
            self.items.pop(item_id, None)
            self.blobs.pop(item_id, None)
            return Response(204, {}, b"")
        if item_id not in self.items:
            return self.response(404, {})
        if method == "PUT":
            existed = item_id in self.blobs
            self.blobs[item_id] = body
            self.items[item_id]["state"] = "ready"
            return self.response(200 if existed else 201, {"item": self.items[item_id]})
        if item_id in self.content_override:
            return self.content_override[item_id]
        if self.items[item_id]["state"] == "consumed":
            # The real server releases the bytes on ACK and answers exactly this body.
            return self.response(410, {"error": "content released"})
        data = self.blobs[item_id]
        return Response(200, {"x-reader-sha256": self.items[item_id]["sha256"],
                              "content-length": str(len(data))}, data)

    def ordered_annotations(self):
        ordered = []
        for number, record in enumerate(self.annotations.values(), 1):
            ordered.append({**record, "sequence": number, "received_at": NOW})
        ordered.sort(key=lambda record: record["sequence"])
        return ordered

    @staticmethod
    def response(status, value):
        return Response(status, {}, json_bytes(value))


class CloudSyncTest(unittest.TestCase):
    def setUp(self):
        self.environment = patch.dict("os.environ", {"READER_SYNC_KEY": KEY}, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.root = Path(tempfile.mkdtemp(prefix="reader-send-sync-test-"))
        self.addCleanup(lambda: __import__("shutil").rmtree(self.root, ignore_errors=True))
        self.cache = Cache(self.root / "cache")
        self.api = FakeAPI()
        self.counter = 0

    def nonce(self):
        self.counter += 1
        return f"{self.counter:032x}"

    def client(self):
        return CloudClient(transport=self.api, clock=lambda: NOW, nonce=self.nonce)

    def store(self, identity="a" * 24, title="Cached book", data=None):
        data = epub_bytes() if data is None else data
        article = ReadingItem(title=title, author="", source_url="https://example.com/post",
                              date="", language="", content="", images=[])
        return self.cache.store(identity, article, data)

    def ready(self, data=None, **changes):
        data = epub_bytes() if data is None else data
        record = item(data, **changes)
        self.api.items[record["id"]] = {**record, "state": "ready",
                                        "created_at": NOW * 1000, "consumed_at": None}
        self.api.blobs[record["id"]] = data
        return record

    def routes(self):
        return [(method, route.partition("?")[0]) for method, route, _ in self.api.calls]

    def test_push_is_idempotent_and_never_rewrites_a_conflicting_id(self):
        self.store()
        first = sync(self.client(), self.cache, push=True, pull=False)
        self.assertEqual(len(first["push"]["pushed"]), 1)
        item_id = first["push"]["pushed"][0]["item_id"]
        self.assertEqual(self.api.items[item_id]["state"], "ready")
        puts = self.routes().count(("PUT", f"/reader/items/{item_id}/content"))
        second = sync(self.client(), self.cache, push=True, pull=False)
        self.assertEqual(second["push"]["pushed"], [])
        self.assertEqual(second["push"]["already_synced"], [{"cache_id": "a" * 24, "item_id": item_id, "receipt": True}])
        self.assertEqual(self.routes().count(("PUT", f"/reader/items/{item_id}/content")), puts)
        self.api.items[item_id]["sha256"] = "b" * 64
        third = sync(self.client(), self.cache, push=True, pull=False)
        self.assertEqual(len(third["push"]["conflicts"]), 1)
        self.assertEqual(third["push"]["pushed"], [])
        self.assertIn("never rewritten", third["push"]["conflicts"][0]["reason"])
        self.assertEqual(self.api.items[item_id]["sha256"], "b" * 64)

    def test_push_completes_an_unfinished_reservation(self):
        self.store()
        metadata, data = prepare_push(self.cache, "a" * 24)
        self.api.items[metadata["id"]] = {**metadata, "state": "uploading",
                                          "created_at": NOW * 1000, "consumed_at": None}
        result = sync(self.client(), self.cache, push=True, pull=False)
        self.assertEqual([entry["item_id"] for entry in result["push"]["pushed"]], [metadata["id"]])
        self.assertEqual(result["push"]["already_synced"], [])
        self.assertEqual(self.api.items[metadata["id"]]["state"], "ready")
        self.assertEqual(self.api.blobs[metadata["id"]], data)

    def test_push_is_bounded_and_reports_deferred_work(self):
        for identity in ("a" * 24, "b" * 24, "c" * 24):
            self.store(identity=identity, title="Book " + identity[0])
        first = sync(self.client(), self.cache, push=True, pull=False, max_push=2)
        self.assertEqual(len(first["push"]["pushed"]), 2)
        self.assertEqual(first["push"]["deferred"], 1)
        second = sync(self.client(), self.cache, push=True, pull=False, max_push=2)
        self.assertEqual(len(second["push"]["pushed"]), 1)
        self.assertEqual(second["push"]["deferred"], 0)
        self.assertEqual(len(self.api.items), 3)

    def test_pull_installs_a_verified_cache_entry_and_skips_known_bytes(self):
        record = self.ready()
        first = sync(self.client(), self.cache, push=False, pull=True)
        self.assertEqual([entry["item_id"] for entry in first["pull"]["pulled"]], [ITEM_ID])
        installed = self.cache.get(cache_id_for(ITEM_ID))
        self.assertEqual(cache_id_for(ITEM_ID), installed["id"])
        self.assertEqual(installed["sha256"], record["sha256"])
        self.assertEqual(installed["size"], record["size"])
        self.assertEqual(installed["source_id"], "cloud:" + ITEM_ID)
        self.assertEqual(installed["source_type"], "cloud")
        self.assertEqual(Path(installed["path"]).read_bytes(), self.api.blobs[ITEM_ID])
        downloads = self.routes().count(("GET", f"/reader/items/{ITEM_ID}/content"))
        second = sync(self.client(), self.cache, push=False, pull=True)
        self.assertEqual(second["pull"]["already_cached"], [ITEM_ID])
        self.assertEqual(second["pull"]["pulled"], [])
        self.assertEqual(self.routes().count(("GET", f"/reader/items/{ITEM_ID}/content")), downloads)

    def consume_after_listing(self, item_id):
        """Another client's ACK lands after our listing but before our download."""
        def acknowledge():
            self.api.items[item_id]["state"] = "consumed"
            self.api.items[item_id]["consumed_at"] = NOW * 1000
            self.api.blobs.pop(item_id)
        self.api.after_list = acknowledge

    def test_item_acknowledged_after_listing_is_released_and_the_pass_continues(self):
        self.ready(epub_bytes(b"<p>first</p>"))
        second = self.ready(epub_bytes(b"<p>second</p>"), id=OTHER_ID)
        self.consume_after_listing(ITEM_ID)
        result = sync(self.client(), self.cache, push=False, pull=True)
        self.assertEqual(result["pull"]["released"], [ITEM_ID])
        self.assertEqual([entry["item_id"] for entry in result["pull"]["pulled"]], [OTHER_ID])
        self.assertEqual(result["pull"]["deferred"], 0)
        self.assertEqual([entry["sha256"] for entry in self.cache.entries()], [second["sha256"]])
        self.assertIsNone(self.cache.get(cache_id_for(ITEM_ID), verify=False))
        self.assertFalse([call for call in self.routes() if call[1].endswith("/ack")])
        self.assertEqual(self.api.items[ITEM_ID]["state"], "consumed")

    def test_released_item_in_incoming_directory_pass_writes_nothing_for_it(self):
        self.ready(epub_bytes(b"<p>first</p>"))
        self.ready(epub_bytes(b"<p>second</p>"), id=OTHER_ID)
        incoming = self.root / "incoming"
        self.consume_after_listing(ITEM_ID)
        result = sync(self.client(), self.cache, push=False, pull=True, incoming_dir=incoming)
        self.assertEqual(result["pull"]["released"], [ITEM_ID])
        self.assertEqual(sorted(path.name for path in incoming.iterdir()),
                         [OTHER_ID + ".epub", OTHER_ID + ".json"])
        self.assertEqual(self.cache.entries(), [])

    def test_drain_survives_a_release_race_and_keeps_annotations(self):
        self.ready(epub_bytes(b"<p>first</p>"))
        self.ready(epub_bytes(b"<p>second</p>"), id=OTHER_ID)
        self.api.annotations[annotation(1)["id"]] = annotation(1)
        self.consume_after_listing(ITEM_ID)
        output = self.root / "annotations.ndjson"
        result = drain(self.client(), self.cache, annotations_out=output, wait=lambda seconds: None)
        self.assertTrue(result["complete"])
        self.assertIsNone(result["stopped"])
        self.assertEqual(result["pull"], {"pulled": 1, "already_cached": 0, "released": 1})
        self.assertIsNone(result["annotations_error"])
        self.assertEqual([json.loads(line)["id"] for line in output.read_text().splitlines()],
                         [annotation(1)["id"]])

    def test_only_the_confirmed_release_response_is_tolerated(self):
        self.ready(epub_bytes(b"<p>first</p>"))
        self.ready(epub_bytes(b"<p>second</p>"), id=OTHER_ID)
        for override, message in ((FakeAPI.response(410, {}), "HTTP 410"),
                                  (FakeAPI.response(410, {"error": "gone"}), "HTTP 410"),
                                  (Response(410, {}, b"not json"), "HTTP 410"),
                                  (FakeAPI.response(404, {}), "HTTP 404")):
            with self.subTest(status=override.status, body=override.data):
                self.api.content_override = {ITEM_ID: override}
                with self.assertRaisesRegex(ReaderError, message) as raised:
                    sync(self.client(), self.cache, push=False, pull=True)
                self.assertNotIsInstance(raised.exception, CloudContentReleased)
                self.assertEqual(self.cache.entries(), [])
                self.assertNotIn(("GET", f"/reader/items/{OTHER_ID}/content"), self.routes())
                self.api.calls.clear()

    def test_drained_pull_is_never_pushed_back_as_a_duplicate(self):
        self.ready()
        first = sync(self.client(), self.cache, push=True, pull=True)
        self.assertEqual([entry["item_id"] for entry in first["pull"]["pulled"]], [ITEM_ID])
        posts = self.routes().count(("POST", "/reader/items"))
        second = sync(self.client(), self.cache, push=True, pull=True)
        self.assertEqual(second["push"]["pushed"], [])
        self.assertEqual(second["push"]["from_cloud"], [cache_id_for(ITEM_ID)])
        self.assertEqual(second["push"]["deferred"], 0)
        self.assertEqual(self.routes().count(("POST", "/reader/items")), posts)
        self.assertEqual(list(self.api.items), [ITEM_ID])

    def test_pull_to_incoming_directory_is_verified_and_idempotent(self):
        record = self.ready()
        incoming = self.root / "incoming"
        first = sync(self.client(), self.cache, push=False, pull=True, incoming_dir=incoming)
        self.assertEqual(len(first["pull"]["pulled"]), 1)
        book = incoming / (ITEM_ID + ".epub")
        sidecar = incoming / (ITEM_ID + ".json")
        self.assertEqual(book.read_bytes(), self.api.blobs[ITEM_ID])
        self.assertEqual(json.loads(sidecar.read_bytes())["sha256"], record["sha256"])
        self.assertEqual(self.cache.entries(), [])
        second = sync(self.client(), self.cache, push=False, pull=True, incoming_dir=incoming)
        self.assertEqual(second["pull"]["already_cached"], [ITEM_ID])
        book.write_bytes(epub_bytes(b"<p>tampered</p>"))
        with self.assertRaisesRegex(ReaderError, "differs from the cloud copy"):
            sync(self.client(), self.cache, push=False, pull=True, incoming_dir=incoming)

    def test_dry_run_lists_but_writes_nothing(self):
        self.store()
        # Different bytes than the cached entry, so the pull is planned rather than skipped.
        self.ready(epub_bytes(b"<p>remote only</p>"))
        before = {path: path.read_bytes() for path in self.root.rglob("*") if path.is_file()}
        result = sync(self.client(), self.cache, push=True, pull=True, dry_run=True)
        self.assertTrue(result["dry_run"])
        self.assertTrue(result["push"]["pushed"][0]["planned"])
        self.assertTrue(result["pull"]["pulled"][0]["planned"])
        self.assertEqual(self.routes(), [("GET", "/reader/items")])
        self.assertEqual(before, {path: path.read_bytes() for path in self.root.rglob("*") if path.is_file()})

    def test_annotation_return_and_export_use_existing_batches(self):
        source = self.root / "native.json"
        source.write_text(json.dumps(annotation(1)))
        client = self.client()
        returned = sync(client, self.cache, push=False, pull=False, annotations_in=source)
        self.assertEqual(returned["annotations_in"], {"accepted": 1, "inserted": 1, "batches": 1})
        again = sync(self.client(), self.cache, push=False, pull=False, annotations_in=source)
        self.assertEqual(again["annotations_in"]["inserted"], 0)
        ndjson = self.root / "out.ndjson"
        exported = sync(self.client(), self.cache, push=False, pull=False, annotations_out=ndjson)
        self.assertEqual(exported["annotations_out"]["count"], 1)
        self.assertTrue(exported["annotations_out"]["written"])
        lines = [json.loads(line) for line in ndjson.read_text().splitlines() if line.strip()]
        self.assertEqual([line["id"] for line in lines], [annotation(1)["id"]])
        as_json = self.root / "out.json"
        sync(self.client(), self.cache, push=False, pull=False, annotations_out=as_json)
        self.assertEqual(json.loads(as_json.read_text())["annotations"][0]["kind"], "quote")
        calls = len(self.api.calls)
        dry = sync(self.client(), self.cache, push=False, pull=False,
                   annotations_out=self.root / "dry.ndjson", dry_run=True)
        self.assertTrue(dry["annotations_out"]["planned"])
        self.assertFalse(dry["annotations_out"]["written"])
        self.assertFalse((self.root / "dry.ndjson").exists())
        # dry-run performs no annotation request; only the single read-only inbox listing.
        self.assertEqual([(method, route.partition("?")[0], body) for method, route, body in self.api.calls[calls:]],
                         [("GET", "/reader/items", b"")])

    def test_managed_native_wrapper_round_trip(self):
        device = "9f1d6c3e-4b2a-4f77-8a11-2b3c4d5e6f70"
        native = native_annotation()
        wrapper = self.root / "reading-inbox.json"
        wrapper.write_text(json.dumps({"format": "reader-workbench-native-v1",
                                       "device_id": device, "annotations": [native]}))
        returned = sync(self.client(), self.cache, push=False, pull=False,
                        annotations_in=wrapper, device_id=device)
        self.assertEqual(returned["annotations_in"], {"accepted": 1, "inserted": 1, "batches": 1})
        (record,) = self.api.annotations.values()
        anchor = json.loads(record["source_anchor"])
        self.assertEqual(anchor["format"], "reader-workbench-native-v1")
        self.assertEqual(anchor["device_id"], device)
        self.assertEqual(anchor["native"], {key: value for key, value in native.items()
                                            if key not in {"excerpt", "note"}})
        self.assertNotIn("excerpt", anchor["native"])
        self.assertEqual(record["selection"], " exact card selection ")
        self.assertEqual(record["note"], "return this note unchanged")
        self.assertEqual(record["kind"], "question")
        self.assertEqual(record["book_path"], "/books/card-source.epub")
        self.assertEqual(record["created_at"], NOW * 1000)
        self.assertIsNone(record["item_id"])
        # Deterministic IDs: an unchanged retry is accepted but inserts nothing.
        again = sync(self.client(), self.cache, push=False, pull=False,
                     annotations_in=wrapper, device_id=device)
        self.assertEqual(again["annotations_in"], {"accepted": 1, "inserted": 0, "batches": 1})
        # Without the device label a native wrapper is refused; the listing is the only request.
        calls = len(self.api.calls)
        with self.assertRaisesRegex(ReaderError, "--device-id"):
            sync(self.client(), self.cache, push=False, pull=False, annotations_in=wrapper)
        self.assertEqual([(method, route.partition("?")[0], body) for method, route, body in self.api.calls[calls:]],
                         [("GET", "/reader/items", b"")])

    def test_empty_managed_wrapper_is_refused_before_upload(self):
        wrapper = self.root / "reading-inbox-empty.json"
        wrapper.write_text(json.dumps({"format": "reader-workbench-native-v1",
                                       "device_id": "9f1d6c3e-4b2a-4f77-8a11-2b3c4d5e6f70",
                                       "annotations": []}))
        with self.assertRaisesRegex(ReaderError, "No annotation records found"):
            sync(self.client(), self.cache, push=False, pull=False,
                 annotations_in=wrapper, annotations_out=self.root / "out.ndjson")
        self.assertEqual(self.routes(), [("GET", "/reader/items")])
        self.assertFalse((self.root / "out.ndjson").exists())

    def test_preexisting_cloud_item_and_annotation_are_preserved(self):
        keep_item = self.ready(id=ITEM_ID, **{"title": "Older remote book"})
        self.api.annotations[annotation(7)["id"]] = annotation(7)
        self.store()
        result = sync(self.client(), self.cache, push=True, pull=True)
        self.assertEqual(len(result["push"]["pushed"]), 1)
        self.assertEqual(result["pull"]["already_cached"], [ITEM_ID])
        self.assertIn(ITEM_ID, self.api.items)
        self.assertEqual(self.api.items[ITEM_ID]["sha256"], keep_item["sha256"])
        self.assertIn(annotation(7)["id"], self.api.annotations)
        for method, _ in self.routes():
            self.assertNotIn(method, {"DELETE"})
        self.assertNotIn("/reader/items/" + ITEM_ID + "/ack", [route for _, route in self.routes()])

    def test_invalid_selection_and_corrupt_cache_fail_before_network(self):
        with self.assertRaisesRegex(ReaderError, "nothing selected"):
            sync(self.client(), self.cache, push=False, pull=False)
        with self.assertRaisesRegex(ReaderError, "max_push"):
            sync(self.client(), self.cache, push=True, pull=False, max_push=0)
        with self.assertRaisesRegex(ReaderError, "CloudClient"):
            sync(object(), self.cache)
        record = self.store()
        Path(record["path"]).write_bytes(epub_bytes(b"<p>edited after caching</p>"))
        with self.assertRaisesRegex(ReaderError, "integrity mismatch"):
            sync(self.client(), self.cache, push=True, pull=False)
        # Only the read-only inbox listing happened: a corrupt cache entry is never uploaded.
        self.assertEqual(self.routes(), [("GET", "/reader/items")])
