import io
import json
import tempfile
import unittest
import zipfile
from email.message import Message
from pathlib import Path
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.parse import parse_qs, urlsplit

from reader_send.common import ReaderError, http_url
from reader_send.localsend import PREFIX, Device, announcement, endpoint, send
from reader_send.network import PublicHTTP, Response


class FakeResponse:
    def __init__(self, status=200, raw=b""):
        self.status = status
        self.raw = raw

    def read(self, limit):
        return self.raw[:limit]


class ProtocolTest(unittest.TestCase):
    def test_endpoint_and_discovery_validation(self):
        self.assertEqual(endpoint("192.0.2.1").port, 53317)
        self.assertEqual(endpoint("[::1]:1234").address, "[::1]:1234")
        for bad in ("https://example.com:53317", "reader:0", "reader:65536", "user:pass@reader", "reader/path", "reader\nX:1"):
            with self.assertRaises(ReaderError):
                endpoint(bad)
        packet = {"alias": "CrossPoint", "version": "2.0", "fingerprint": "reader", "protocol": "http", "port": 53317}
        self.assertEqual(announcement(packet, "192.0.2.2").host, "192.0.2.2")
        self.assertIsNone(announcement(packet, "192.0.2.2", "reader"))
        self.assertIsNone(announcement({**packet, "port": True}, "192.0.2.2"))
        self.assertIsNone(announcement({**packet, "version": "1.0"}, "192.0.2.2"))
        self.assertIsNone(announcement([], "192.0.2.2"))

    def make_epub(self, root):
        path = root / "article.epub"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
            archive.writestr("OEBPS/article.xhtml", "<p>Test article</p>")
        return path

    def test_prepare_upload_wire_contract_and_binary_body(self):
        requests = []
        test = self
        class Connection:
            def __init__(self, host, port, timeout):
                test.assertEqual((host, port), ("192.0.2.1", 53317))
            def request(self, method, route, body=None, headers=None):
                raw = body.read() if hasattr(body, "read") else body
                requests.append((method, route, raw, headers))
                if route.endswith("info"):
                    self.response = FakeResponse(raw=b'{"version":"2.0","alias":"CrossPoint"}')
                elif route.endswith("prepare-upload"):
                    payload = json.loads(raw)
                    file_id, info = next(iter(payload["files"].items()))
                    test.assertEqual(file_id, info["id"])
                    test.assertEqual(info["fileType"], "application/epub+zip")
                    test.assertRegex(info["sha256"], r"^[0-9a-f]{64}$")
                    test.assertEqual(info["fileName"], "article.epub")
                    self.response = FakeResponse(raw=json.dumps({"sessionId": "session +&?", "files": {file_id: "token/+&?"}}).encode())
                else:
                    self.response = FakeResponse()
            def getresponse(self):
                return self.response
            def close(self):
                pass
        with tempfile.TemporaryDirectory() as temporary:
            path = self.make_epub(Path(temporary))
            with patch("reader_send.localsend.http.client.HTTPConnection", Connection):
                result = send(path, Device("192.0.2.1"))
            self.assertEqual(result["status"], "sent")
            self.assertEqual(len(requests), 3)
            method, route, raw, headers = requests[-1]
            self.assertEqual(method, "POST")
            self.assertEqual(urlsplit(route).path, PREFIX + "upload")
            query = parse_qs(urlsplit(route).query)
            self.assertEqual(query["sessionId"], ["session +&?"])
            self.assertEqual(query["token"], ["token/+&?"])
            self.assertEqual(raw, path.read_bytes())
            self.assertEqual(int(headers["Content-Length"]), len(raw))
            self.assertEqual(headers["Content-Type"], "application/octet-stream")
            self.assertNotIn("Transfer-Encoding", headers)

    def test_failure_cancels_without_upload_retry(self):
        calls = []
        def transport(device, method, route, body=None, **kwargs):
            calls.append(route)
            if route == "info":
                return 200, b'{"version":"2.0"}'
            if route == "prepare-upload":
                file_id = next(iter(json.loads(body)["files"]))
                return 200, json.dumps({"sessionId": "s", "files": {file_id: "t"}}).encode()
            if route.startswith("upload?"):
                return 422, b""
            return 204, b""
        with tempfile.TemporaryDirectory() as temporary:
            path = self.make_epub(Path(temporary))
            with patch("reader_send.localsend.request", side_effect=transport):
                with self.assertRaisesRegex(ReaderError, "checksum mismatch"):
                    send(path, Device("192.0.2.1"))
        self.assertEqual(sum(p.startswith("upload?") for p in calls), 1)
        self.assertTrue(calls[-1].startswith("cancel?"))

    def test_dry_run_and_prepare_204_do_not_upload(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = self.make_epub(Path(temporary))
            with patch("reader_send.localsend.request", side_effect=AssertionError("network forbidden")):
                result = send(path, None, dry_run=True)
            self.assertTrue(result["dry_run"])
            with patch("reader_send.localsend.request", side_effect=[(200, b'{"version":"2.0"}'), (204, b"")]) as request:
                result = send(path, Device("192.0.2.1"))
            self.assertEqual(result["status"], "already-present")
            self.assertEqual(request.call_count, 2)

    def test_prepare_rejection_and_missing_token(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = self.make_epub(Path(temporary))
            with patch("reader_send.localsend.request", side_effect=[(200, b'{"version":"2.0"}'), (409, b"")]):
                with self.assertRaisesRegex(ReaderError, "busy"):
                    send(path, Device("192.0.2.1"))
            with patch("reader_send.localsend.request", side_effect=[(200, b'{"version":"2.0"}'), (200, b'{"sessionId":"s","files":{}}'), (204, b"")]) as request:
                with self.assertRaisesRegex(ReaderError, "did not accept"):
                    send(path, Device("192.0.2.1"))
                self.assertEqual(request.call_count, 3)

    def test_invalid_epub_fails_before_network(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "bad.epub"
            path.write_bytes(b"not a zip")
            with patch("reader_send.localsend.request", side_effect=AssertionError("network forbidden")):
                with self.assertRaisesRegex(ReaderError, "Not an EPUB"):
                    send(path, Device("192.0.2.1"))


class PublicHTTPTest(unittest.TestCase):
    def test_credentials_and_unsupported_schemes_rejected(self):
        for url in ("file:///etc/passwd", "https://name:secret@example.com/", "javascript:alert(1)", "https://example.com/\n"):
            with self.assertRaises(ReaderError):
                http_url(url)

    def test_international_urls_are_normalized_for_http(self):
        self.assertEqual(http_url("https://例子.测试/文章?q=你好"),
                         "https://xn--fsqu00a.xn--0zwm56d/%E6%96%87%E7%AB%A0?q=%E4%BD%A0%E5%A5%BD")
        self.assertEqual(http_url("http://[::1]:53317/a%20b"), "http://[::1]:53317/a%20b")
        with self.assertRaises(ReaderError):
            http_url("https://example.com/" + chr(0xD800))

    def test_robots_blocks_without_fetching_article(self):
        client = PublicHTTP()
        with patch.object(client, "_request", return_value=Response(b"User-agent: *\nDisallow: /private\n", "https://example.com/robots.txt", "text/plain", None)) as request:
            with self.assertRaisesRegex(ReaderError, "robots.txt disallows"):
                client.get("https://example.com/private/post")
            self.assertEqual(request.call_count, 1)

    def test_missing_robots_allows_public_article(self):
        client = PublicHTTP()
        missing = HTTPError("https://example.com/robots.txt", 404, "not found", {}, io.BytesIO())
        response = Response(b"<p>Text</p>", "https://example.com/post", "text/html", None)
        with patch.object(client, "_request", side_effect=[missing, response]):
            self.assertEqual(client.get("https://example.com/post"), response)

    def test_temporary_get_failure_retried_only_within_bound(self):
        client = PublicHTTP(retries=1)
        class HTTPResult(io.BytesIO):
            url = "https://example.com/robots.txt"
            headers = Message()
        with patch.object(client.opener, "open", side_effect=[HTTPError("https://example.com/robots.txt", 503, "unavailable", {}, io.BytesIO()), HTTPResult(b"ok")]) as opened, patch("reader_send.network.time.sleep"):
            self.assertEqual(client._request("https://example.com/robots.txt", 100, False).data, b"ok")
            self.assertEqual(opened.call_count, 2)
        with patch.object(client.opener, "open", return_value=HTTPResult(b"too long")):
            with self.assertRaisesRegex(ReaderError, "exceeds"):
                client._request("https://example.com/robots.txt", 2, False)


if __name__ == "__main__":
    unittest.main()
