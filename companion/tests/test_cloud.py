import base64
import hashlib
import hmac
import io
import json
import ssl
import tempfile
import unittest
import uuid
import zipfile
from pathlib import Path
from unittest.mock import patch

from reader_send.cache import Cache
from reader_send.cli import main, parser
from reader_send.cloud import (MAX_EPUB, CloudClient, Response, https_request, json_bytes,
                               origin_url, prepare_push)
from reader_send.cloud_annotations import (annotation_batches, download_annotations, load_annotations,
                                           map_annotation, upload_annotations)
from reader_send.common import ReaderError
from reader_send.extract import extract

KEY = "0123456789abcdef" * 4
NOW = 1793318400
ITEM_ID = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"


def epub_bytes():
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as archive:
        info = zipfile.ZipInfo("mimetype", (2026, 1, 1, 0, 0, 0))
        archive.writestr(info, b"application/epub+zip")
        info = zipfile.ZipInfo("content.xhtml", (2026, 1, 1, 0, 0, 0))
        archive.writestr(info, b"<p>Byte-identical EPUB</p>")
    return stream.getvalue()


def item(data=None, **changes):
    data = epub_bytes() if data is None else data
    return {"id": ITEM_ID, "title": "A book — 你好", "original_url": "https://example.com/post?x=1#part",
            "source_path": "/books/unchanged.epub", "sha256": hashlib.sha256(data).hexdigest(),
            "size": len(data), **changes}


def annotation(number=1, **changes):
    return {"id": str(uuid.UUID(int=number)), "item_id": None, "book_path": "/books/source.html",
            "source_anchor": ' {"xpath":"/html/body/p[2]","vo":0,"unknown":"保留"} ',
            "selection": "  exact\ntext\t你好 😀  ", "kind": "quote", "note": "\twhy?\n", "created_at": 0,
            **changes}


def native(**changes):
    return {"version": 1, "id": 2, "kind": 2, "sourcePath": "/books/original.html",
            "sourceTitle": "Original", "chapterTitle": "Chapter", "chapterHref": "ch.xhtml",
            "xpath": "/html/body/p[4]", "percentage": 0.25, "si": 1, "pc": 13, "pp": 2,
            "vo": 0, "excerpt": " exact selection ", "note": "optional note", "pageContext": False,
            "createdAt": NOW, **changes}


class MemoryAPI:
    def __init__(self):
        self.calls = []
        self.items = {}
        self.blobs = {}
        self.annotations = {}
        self.nonces = set()

    def __call__(self, origin, method, route, body, headers, timeout, max_bytes):
        self.calls.append((origin, method, route, body, headers))
        assert headers["Authorization"] == "Bearer " + KEY
        canonical = ("reader-sync-v1\n" + method + "\n" + route + "\n" + hashlib.sha256(body).hexdigest()
                     + "\n" + headers["X-Reader-Timestamp"] + "\n" + headers["X-Reader-Nonce"])
        expected = base64.urlsafe_b64encode(hmac.digest(KEY.encode(), canonical.encode(), "sha256")).decode().rstrip("=")
        assert headers["X-Reader-Signature"] == expected
        assert headers["X-Reader-Nonce"] not in self.nonces
        self.nonces.add(headers["X-Reader-Nonce"])
        assert headers["Content-Length"] == str(len(body))
        assert not any(key.lower() in {"cookie", "origin", "content-encoding"} for key in headers)
        if route == "/reader/items":
            if method == "GET":
                return self.response(200, {"items": list(self.items.values())})
            metadata = json.loads(body)
            prior = self.items.get(metadata["id"])
            if prior and any(prior[key] != value for key, value in metadata.items()):
                return self.response(409, {"error": "conflict"})
            if not prior:
                self.items[metadata["id"]] = {**metadata, "state": "uploading", "created_at": NOW * 1000, "consumed_at": None}
            return self.response(200 if prior else 201, {"item": self.items[metadata["id"]]})
        if route == "/reader/annotations":
            values = json.loads(body)["annotations"]
            if any(value["id"] in self.annotations and self.annotations[value["id"]] != value for value in values):
                return self.response(409, {})
            inserted = sum(value["id"] not in self.annotations for value in values)
            self.annotations.update({value["id"]: value for value in values})
            return self.response(201 if inserted else 200, {"accepted": len(values), "inserted": inserted})
        item_id = route.split("/")[3]
        if method == "DELETE":
            self.items.pop(item_id, None)
            self.blobs.pop(item_id, None)
            return Response(204, {}, b"")
        if item_id not in self.items:
            return self.response(404, {})
        if route.endswith("/ack"):
            if item_id not in self.blobs:
                return self.response(409, {})
            self.items[item_id]["state"] = "consumed"
            self.items[item_id]["consumed_at"] = self.items[item_id]["consumed_at"] or NOW * 1000
            return self.response(200, {"item": self.items[item_id]})
        if method == "PUT":
            existed = item_id in self.blobs
            self.blobs[item_id] = body
            if not existed:
                self.items[item_id]["state"] = "ready"
            return self.response(200 if existed else 201, {"item": self.items[item_id]})
        data = self.blobs[item_id]
        return Response(200, {"x-reader-sha256": self.items[item_id]["sha256"], "content-length": str(len(data))}, data)

    @staticmethod
    def response(status, value):
        return Response(status, {}, json_bytes(value))


class CloudTest(unittest.TestCase):
    def setUp(self):
        self.environment = patch.dict("os.environ", {"READER_SYNC_KEY": KEY}, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.api = MemoryAPI()
        self.counter = 0

    def nonce(self):
        self.counter += 1
        return f"{self.counter:032x}"

    def client(self, transport=None):
        return CloudClient(transport=transport or self.api, clock=lambda: NOW, nonce=self.nonce)

    def test_signature_uses_exact_body_query_and_utf8_key_not_decoded_hex(self):
        calls = []
        def transport(*args):
            calls.append(args)
            return Response(200, {}, b"{}")
        client = self.client(transport)
        body = json_bytes({"annotations": [annotation()]})
        client.request("POST", "/reader/annotations", body, "application/json")
        client.request("GET", "/reader/annotations?after=7&limit=32")
        for index, (_, method, route, raw, headers, _, _) in enumerate(calls, 1):
            canonical = "\n".join(["reader-sync-v1", method, route, hashlib.sha256(raw).hexdigest(), str(NOW), f"{index:032x}"])
            signature = base64.urlsafe_b64encode(hmac.new(KEY.encode(), canonical.encode(), hashlib.sha256).digest()).decode().rstrip("=")
            wrong = base64.urlsafe_b64encode(hmac.new(bytes.fromhex(KEY), canonical.encode(), hashlib.sha256).digest()).decode().rstrip("=")
            self.assertEqual(headers["X-Reader-Signature"], signature)
            self.assertNotEqual(signature, wrong)
            self.assertEqual(headers["User-Agent"], "ReaderSend/1")
        self.assertIs(calls[0][3], body)
        self.assertEqual(calls[1][3], b"")
        self.assertEqual(calls[0][4]["Content-Type"], "application/json")

    def test_origins_secrets_and_route_guard(self):
        for bad in ("http://robertzhou.dev", "https://x/a", "https://x?", "https://x#", "https://x?secret=y", "https://u:p@x",
                    "https://x:0", "https://x:65536", "https://x\n", "https://x\\@elsewhere"):
            with self.subTest(origin=bad), self.assertRaises(ReaderError):
                origin_url(bad)
        self.assertEqual(origin_url("https://LOCALHOST:8443/"), "https://localhost:8443")
        for bad in ("", "a" * 63, "A" * 64, "reader-key", "a" * 64 + "\n"):
            with patch.dict("os.environ", {"READER_SYNC_KEY": bad}), self.assertRaisesRegex(ReaderError, "environment variable"):
                CloudClient()
        with patch.dict("os.environ", {"OWNER_KEY": KEY, "SIGNING_KEY": KEY}, clear=True), self.assertRaises(ReaderError):
            CloudClient()
        client = self.client()
        for bad in ("https://elsewhere/reader/items", "//elsewhere/reader/items", "/reader/items?private=title", "/other"):
            with self.assertRaises(ReaderError):
                client.request("GET", bad)
        self.assertEqual(self.api.calls, [])

    def test_https_transport_never_follows_redirect_or_accepts_cookies(self):
        requests = []
        contexts = []
        test = self
        class Connection:
            def __init__(self, host, port, timeout, context):
                test.assertEqual((host, port), ("example.com", 443))
                contexts.append(context)
            def request(self, method, route, body, headers):
                requests.append((method, route, body, headers))
            def getresponse(self):
                return self
            status = 302
            def getheaders(self):
                return [("Location", "https://elsewhere.example/steal"), ("Set-Cookie", "secret=bad")]
            def read(self, limit):
                raise AssertionError("redirect body must not be read")
            def close(self):
                pass
        with patch("reader_send.cloud.http.client.HTTPSConnection", Connection):
            client = CloudClient("https://example.com", clock=lambda: NOW, nonce=self.nonce)
            for _ in range(2):
                with self.assertRaisesRegex(ReaderError, "redirect refused"):
                    client.items()
        self.assertEqual(len(requests), 2)
        self.assertNotEqual(requests[0][3]["X-Reader-Nonce"], requests[1][3]["X-Reader-Nonce"])
        self.assertTrue(all("Cookie" not in request[3] for request in requests))
        self.assertTrue(all(context.check_hostname and context.verify_mode == ssl.CERT_REQUIRED for context in contexts))

    def test_transport_rejects_truncation_size_and_encoding_and_closes(self):
        from unittest.mock import Mock
        for headers, raw in (([("Content-Length", "5")], b"x"), ([("Content-Length", "999")], b""),
                             ([("Content-Length", "9" * 5000)], b""),
                             ([("Content-Encoding", "gzip")], b"compressed"), ([], b"x" * 11)):
            conn = Mock()
            conn.getresponse.return_value.status = 200
            conn.getresponse.return_value.getheaders.return_value = headers
            conn.getresponse.return_value.read.return_value = raw
            with patch("reader_send.cloud.http.client.HTTPSConnection", return_value=conn), self.assertRaises(ReaderError):
                https_request("https://example.com", "GET", "/reader/items", b"", {}, 1, 10)
            conn.close.assert_called_once()
        conn = Mock()
        conn.getresponse.return_value.status = 403
        conn.getresponse.return_value.getheaders.return_value = [("Content-Length", "1000000")]
        conn.getresponse.return_value.read.side_effect = AssertionError("error body must not be read")
        with patch("reader_send.cloud.http.client.HTTPSConnection", return_value=conn), self.assertRaisesRegex(ReaderError, "WAF denied"):
            CloudClient().items()
        conn.close.assert_called_once()

    def test_push_retry_ack_delete_idempotency_and_binary_identity(self):
        data = epub_bytes()
        metadata = item(data)
        client = self.client()
        self.assertEqual(client.push(metadata, data)["state"], "ready")
        self.assertEqual(client.push(metadata, data)["state"], "ready")
        self.assertEqual(len(self.api.items), 1)
        self.assertIs(self.api.blobs[ITEM_ID], data)
        self.assertEqual(client.ack(ITEM_ID), client.ack(ITEM_ID))
        self.assertEqual(client.items(ready_only=True), [])
        self.assertEqual(client.items()[0]["state"], "consumed")
        with self.assertRaisesRegex(ReaderError, "409"):
            client.push(item(title="Changed"), data)
        before = len(self.api.calls)
        client.delete(ITEM_ID)
        client.delete(ITEM_ID)
        self.assertEqual(len(self.api.calls) - before, 2)
        self.assertEqual(self.api.items, {})
        for _, method, route, raw, headers in self.api.calls:
            if method in {"GET", "DELETE"} or route.endswith("/ack"):
                self.assertEqual(raw, b"")
                self.assertNotIn("Content-Type", headers)
        self.assertEqual(len(self.api.nonces), len(self.api.calls))

    def test_api_errors_are_actionable_redacted_and_never_retried(self):
        for status in (400, 401, 403, 404, 405, 409, 413, 415, 422, 429, 503, 507, 500):
            calls = []
            def transport(*args):
                calls.append(args)
                return Response(status, {"retry-after": "60"}, json_bytes({"error": KEY + " private title"}))
            with self.subTest(status=status), self.assertRaises(ReaderError) as failure:
                self.client(transport).items()
            self.assertIn(str(status), str(failure.exception))
            self.assertNotIn(KEY, str(failure.exception))
            self.assertNotIn("private title", str(failure.exception))
            self.assertEqual(len(calls), 1)

    def test_network_failure_does_not_echo_exception_or_retry(self):
        from unittest.mock import Mock
        conn = Mock()
        conn.request.side_effect = OSError(KEY + " private text")
        with patch("reader_send.cloud.http.client.HTTPSConnection", return_value=conn), self.assertRaises(ReaderError) as failure:
            CloudClient().items()
        self.assertNotIn(KEY, str(failure.exception))
        conn.request.assert_called_once()
        conn.close.assert_called_once()

    def test_pull_verifies_all_hashes_before_writing_and_preserves_metadata(self):
        client = self.client()
        client.push(item(), epub_bytes())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = client.pull(ITEM_ID, root)
            self.assertEqual(Path(result["output"]).read_bytes(), epub_bytes())
            self.assertEqual(json.loads(Path(result["metadata"]).read_bytes()), self.api.items[ITEM_ID])
            self.assertFalse(result["acknowledged"])
            self.assertEqual(self.api.items[ITEM_ID]["state"], "ready")
            before = len(self.api.calls)
            with self.assertRaisesRegex(ReaderError, "output exists"):
                client.pull(ITEM_ID, root)
            self.assertEqual(len(self.api.calls), before)
            client.pull(ITEM_ID, root, overwrite=True)
        for tamper in ("body", "hash", "size"):
            def transport(*args):
                response = self.api(*args)
                if args[1] == "GET" and args[2].endswith("/content"):
                    headers = dict(response.headers)
                    if tamper == "hash":
                        headers["x-reader-sha256"] = "0" * 64
                    elif tamper == "size":
                        headers["content-length"] = "1"
                    return Response(200, headers, response.data[:-1] + b"X" if tamper == "body" else response.data)
                return response
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                with self.assertRaisesRegex(ReaderError, "verification failed"):
                    self.client(transport).pull(ITEM_ID, root)
                self.assertEqual(list(root.iterdir()), [])

    def test_cached_push_stable_metadata_and_changed_content_new_uuid(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary))
            article = extract("<article><h1>A</h1><p>Text.</p></article>", "https://example.com/a#section")
            cache_id = cache.key("identity", {})
            original = cache.store(cache_id, article, epub_bytes())
            first, data = prepare_push(cache, cache_id)
            cache.mark(cache_id, "read", 1)
            second, repeated = prepare_push(cache, cache_id)
            self.assertEqual(first, second)
            self.assertEqual(data, repeated)
            self.assertEqual(first["original_url"], original["source_url"])
            self.assertEqual(first["source_path"], original["filename"])
            self.assertNotIn(temporary, json.dumps(first))
            self.assertEqual(cache.get(cache_id)["source_id"], original["source_id"])
            cache.store(cache_id, article, epub_bytes() + b"new bytes")
            self.assertNotEqual(prepare_push(cache, cache_id)[0]["id"], first["id"])
            self.assertEqual(prepare_push(cache, cache_id, "/books/stable.epub")[0]["source_path"], "/books/stable.epub")

    def test_invalid_oversized_and_corrupt_cached_epub_fail_before_network(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary))
            article = extract("<article><p>Text.</p></article>", "https://example.com/a")
            cache_id = cache.key("identity", {})
            for data in (b"not epub", epub_bytes() + b"x" * MAX_EPUB):
                cache.store(cache_id, article, data)
                with self.assertRaises(ReaderError):
                    prepare_push(cache, cache_id)
            record = cache.store(cache_id, article, epub_bytes())
            Path(record["path"]).write_bytes(epub_bytes() + b"tamper")
            with self.assertRaisesRegex(ReaderError, "integrity mismatch"):
                prepare_push(cache, cache_id)
        self.assertEqual(self.api.calls, [])

    def test_cloud_size_boundary_and_malformed_server_metadata(self):
        data = epub_bytes()
        exact = data + b"\0" * (MAX_EPUB - len(data))
        self.assertEqual(self.client().push(item(exact), exact)["size"], MAX_EPUB)
        for payload in ({"items": [{**item(), "state": {}}]}, {"items": [None]},
                        {"items": [{**item(), "state": "ready", "size": True}]}):
            with self.assertRaises(ReaderError):
                self.client(lambda *args: MemoryAPI.response(200, payload)).items()
        for raw in (b'{"items":[],"items":[]}', b'{"items":NaN}', b"\xff"):
            with self.assertRaises(ReaderError):
                self.client(lambda *args: Response(200, {}, raw)).items()

    def test_failed_push_does_not_send_content_or_delete_reservation(self):
        calls = []
        def mismatched(*args):
            calls.append(args)
            return MemoryAPI.response(201, {"item": item(title="unexpected", state="uploading")})
        with self.assertRaisesRegex(ReaderError, "different immutable"):
            self.client(mismatched).push(item(), epub_bytes())
        self.assertEqual(len(calls), 1)
        calls.clear()
        def failed_upload(*args):
            calls.append(args)
            return self.api(*args) if args[1] == "POST" else Response(503, {}, b"{}")
        with self.assertRaisesRegex(ReaderError, "503"):
            self.client(failed_upload).push(item(), epub_bytes())
        self.assertEqual([call[1] for call in calls], ["POST", "PUT"])
        self.assertEqual(self.api.items[ITEM_ID]["state"], "uploading")
        self.assertEqual(self.client().push(item(), epub_bytes())["state"], "ready")

    def test_no_network_cloud_cli_dry_runs_do_not_require_key_or_write(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = Cache(root / "cache")
            cache_id = cache.key("identity", {})
            record = cache.store(cache_id, extract("<p>Text.</p>", "https://example.com/a"), epub_bytes())
            annotations = root / "annotations.json"
            annotations.write_bytes(json_bytes([annotation()]))
            before = {str(path): path.read_bytes() for path in root.rglob("*") if path.is_file()}
            with patch.dict("os.environ", {}, clear=True), patch("socket.socket", side_effect=AssertionError("network forbidden")), patch("sys.stdout", new_callable=io.StringIO) as output:
                self.assertEqual(main(["cloud", "push", cache_id, "--cache-dir", str(cache.root), "--dry-run"]), 0)
                self.assertEqual(json.loads(output.getvalue())["item"]["sha256"], record["sha256"])
                output.seek(0)
                output.truncate()
                self.assertEqual(main(["cloud", "annotations", "upload", str(annotations), "--dry-run"]), 0)
                self.assertTrue(json.loads(output.getvalue())["dry_run"])
            self.assertEqual(before, {str(path): path.read_bytes() for path in root.rglob("*") if path.is_file()})
        args = parser().parse_args(["cloud", "--origin", "https://example.com", "annotations", "download"])
        self.assertEqual(args.origin, "https://example.com")


class CloudAnnotationTest(unittest.TestCase):
    setUp = CloudTest.setUp
    nonce = CloudTest.nonce
    client = CloudTest.client

    def test_native_mapping_preserves_every_field_and_utc_seconds(self):
        source = native()
        mapped = map_annotation(source, device_id="my-reader", now=NOW)
        anchor = json.loads(mapped["source_anchor"])
        restored = {**anchor["native"], "excerpt": mapped["selection"], "note": mapped["note"]}
        self.assertEqual(restored, source)
        self.assertEqual(mapped["kind"], "review_later")
        self.assertEqual(mapped["book_path"], source["sourcePath"])
        self.assertEqual(mapped["created_at"], NOW * 1000)
        self.assertEqual(mapped, map_annotation(source, device_id="my-reader", now=NOW))
        self.assertNotEqual(mapped["id"], map_annotation(source, device_id="other-reader", now=NOW)["id"])
        self.assertNotEqual(mapped["id"], map_annotation(native(note="edited"), device_id="my-reader", now=NOW)["id"])
        no_offset = {key: value for key, value in source.items() if key != "vo"}
        mapped_no_offset = map_annotation(no_offset, device_id="my-reader", now=NOW)
        self.assertNotIn("vo", json.loads(mapped_no_offset["source_anchor"])["native"])
        self.assertEqual(anchor["native"]["vo"], 0)
        self.assertEqual(map_annotation(native(createdAt=0), device_id="my-reader", now=NOW)["created_at"], 0)
        with self.assertRaisesRegex(ReaderError, "device-id"):
            map_annotation(source, now=NOW)

    def test_explicit_categories_and_opaque_anchor_and_full_location(self):
        for kind, expected in ((0, "quote"), (1, "question"), (2, "review_later"), ("Quote", "quote"),
                               ("AnnotationKind::Question", "question"), ("ReviewLater", "review_later"),
                               ("review_later", "review_later")):
            result = map_annotation(annotation(kind=kind), now=NOW)
            self.assertEqual(result["kind"], expected)
            self.assertEqual(result["source_anchor"], annotation()["source_anchor"])
            self.assertEqual(result["selection"], annotation()["selection"])
            self.assertEqual(result["note"], annotation()["note"])
        for bad in (True, 3, None, "highlight", "review"):
            with self.assertRaises(ReaderError):
                map_annotation(annotation(kind=bad), now=NOW)
        location = {"chapterHref": "ch.xhtml", "spineIndex": 0, "xpath": "/html/body/p[1]",
                    "visibleOffset": 0, "percentage": 0.25, "native_extra": {"anything": "保留"}}
        value = annotation()
        del value["source_anchor"]
        value["location"] = location
        self.assertEqual(json.loads(map_annotation(value, now=NOW)["source_anchor"]), location)
        with self.assertRaisesRegex(ReaderError, "not both"):
            map_annotation({**value, "source_anchor": "opaque"}, now=NOW)

    def test_limits_unknown_fields_and_no_truncation(self):
        for changes in ({"selection": "😀" * 4097}, {"source_anchor": "a" * 2049},
                        {"note": "a" * 8193}, {"book_path": ""}, {"created_at": NOW * 1000 + 300001},
                        {"created_at": True}, {"selection": "\x00"}, {"note": "\ud800"},
                        {"unknown": "not supported"}, {"id": 12}):
            with self.subTest(fields=tuple(changes)), self.assertRaises(ReaderError):
                annotation_batches([annotation(**changes)], now=NOW)
        self.assertEqual(map_annotation(annotation(selection="😀" * 4096), now=NOW)["selection"], "😀" * 4096)
        self.assertEqual(map_annotation(annotation(created_at=NOW * 1000 + 300000), now=NOW)["created_at"], NOW * 1000 + 300000)
        with self.assertRaisesRegex(ReaderError, "source_anchor"):
            annotation_batches([native(xpath="a" * 2048)], device_id="reader", now=NOW)

    def test_batch_count_byte_limits_and_duplicate_validation(self):
        records = [annotation(number) for number in range(1, 66)]
        batches = annotation_batches(records, now=NOW)
        self.assertEqual([len(json.loads(body)["annotations"]) for body in batches], [32, 32, 1])
        large = [annotation(number, selection="你" * 8192, note="你" * 8192) for number in range(1, 4)]
        bodies = annotation_batches(large, now=NOW)
        self.assertEqual(len(bodies), 3)
        self.assertTrue(all(len(body) <= 65536 for body in bodies))
        self.assertEqual(json.loads(bodies[0])["annotations"][0]["selection"], large[0]["selection"])
        repeated = annotation_batches([records[0], records[0]], now=NOW)
        self.assertEqual(len(json.loads(repeated[0])["annotations"]), 1)
        with self.assertRaisesRegex(ReaderError, "Conflicting annotation IDs"):
            annotation_batches([annotation(), annotation(note="different")], now=NOW)
        escaped = annotation(selection="\n" * 8192, note="\n" * 8192,
                             source_anchor="😀" * 1024, book_path="😀" * 512)
        encoded = annotation_batches([escaped], now=NOW)
        self.assertLessEqual(len(encoded[0]), 65536)
        self.assertEqual(json.loads(encoded[0])["annotations"], [escaped])

    def test_annotation_idempotency_and_partial_failure_are_actionable(self):
        client = self.client()
        bodies = annotation_batches([annotation(1), annotation(2)], now=NOW)
        self.assertEqual(upload_annotations(client, bodies), {"accepted": 2, "inserted": 2, "batches": 1})
        self.assertEqual(upload_annotations(client, bodies), {"accepted": 2, "inserted": 0, "batches": 1})
        mixed = annotation_batches([annotation(3), annotation(1, note="changed")], now=NOW)
        with self.assertRaisesRegex(ReaderError, "409"):
            upload_annotations(client, mixed)
        self.assertNotIn(annotation(3)["id"], self.api.annotations)
        count = 0
        def transport(*args):
            nonlocal count
            count += 1
            if count == 2:
                return Response(429, {}, b"{}")
            return self.api(*args)
        bodies = annotation_batches([annotation(number) for number in range(10, 43)], now=NOW)
        with self.assertRaisesRegex(ReaderError, "after 32 confirmed records"):
            upload_annotations(self.client(transport), bodies)
        self.assertEqual(count, 2)

    def test_json_and_ndjson_receive_sequence_pagination_preserves_records(self):
        records = [{**annotation(1, created_at=1000), "sequence": 7, "received_at": 1100},
                   {**annotation(2, created_at=500), "sequence": 11, "received_at": 1200}]
        for export in (False, True):
            calls = []
            raw_pages = [json_bytes(record) + b"\n" for record in records]
            def transport(*args):
                calls.append(args)
                index = len(calls) - 1
                if export:
                    return Response(200, {"x-reader-next-after": "7"} if index == 0 else {}, raw_pages[index])
                return MemoryAPI.response(200, {"annotations": [records[index]], "next_after": 7 if index == 0 else None})
            result = download_annotations(self.client(transport), after=3, limit=1, export=export)
            self.assertEqual(result["annotations"], records)
            self.assertEqual(result["after"], 11)
            self.assertIsNone(result["next_after"])
            self.assertTrue(calls[0][2].endswith("?after=3&limit=1"))
            self.assertTrue(calls[1][2].endswith("?after=7&limit=1"))
            if export:
                self.assertEqual(result["ndjson"], b"".join(raw_pages))
        empty = self.client(lambda *args: MemoryAPI.response(200, {"annotations": [], "next_after": None}))
        self.assertEqual(download_annotations(empty, after=11)["after"], 11)

    def test_malformed_pagination_and_annotations_fail_without_looping(self):
        record = {**annotation(), "sequence": 1, "received_at": 2}
        for page in ({"annotations": [], "next_after": 1},
                     {"annotations": [record], "next_after": 2},
                     {"annotations": [record, record], "next_after": None},
                     {"annotations": [{**record, "source_anchor": None}], "next_after": None},
                     {"annotations": [{**record, "source_anchor": {"not": "a string"}}], "next_after": None},
                     {"annotations": [record]},
                     {"annotations": [{**record, "sequence": True}], "next_after": None}):
            calls = []
            def transport(*args):
                calls.append(args)
                return MemoryAPI.response(200, page)
            with self.assertRaises(ReaderError):
                download_annotations(self.client(transport))
            self.assertEqual(len(calls), 1)
        with self.assertRaisesRegex(ReaderError, "Incomplete"):
            download_annotations(self.client(lambda *args: Response(200, {}, json_bytes(record))), export=True)
        with self.assertRaisesRegex(ReaderError, "pagination header"):
            download_annotations(self.client(lambda *args: Response(200, {"x-reader-next-after": "9" * 5000}, b"")), export=True)

    def test_annotation_input_formats_native_directory_and_export_cli(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sources = [
                ("single.json", json_bytes(annotation())),
                ("array.json", json_bytes([annotation()])),
                ("wrapped.json", json_bytes({"annotations": [annotation()], "after": 2})),
                ("rows.ndjson", json_bytes(annotation()) + b"\n"),
            ]
            for name, body in sources:
                path = root / name
                path.write_bytes(body)
                self.assertEqual(load_annotations(path), [annotation()])
            store = root / "native"
            store.mkdir()
            (store / "001.json").write_bytes(json_bytes(native()))
            (store / "001.tmp").write_bytes(b"bad")
            (store / "001.bak").write_bytes(b"bad")
            self.assertEqual(load_annotations(store), [native()])
            record = {**annotation(), "sequence": 7, "received_at": 1000}
            raw = json_bytes(record) + b"\n"
            output = root / "export.ndjson"
            with patch("reader_send.cloud.https_request", return_value=Response(200, {}, raw)), patch("sys.stdout", new_callable=io.StringIO):
                self.assertEqual(main(["cloud", "annotations", "export", "--output", str(output)]), 0)
            self.assertEqual(output.read_bytes(), raw)
            with patch("reader_send.cloud.https_request", side_effect=AssertionError("network forbidden")), patch("sys.stderr", new_callable=io.StringIO):
                self.assertEqual(main(["cloud", "annotations", "export", "--output", str(output)]), 2)


if __name__ == "__main__":
    unittest.main()
