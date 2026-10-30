import io
import json
import os
import ssl
import tempfile
import threading
import unittest
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from unittest.mock import patch

from reader_send.assist import (MAX_NOTE, MAX_PASSAGE, MAX_RESPONSE_BYTES, complete,
                                configuration, endpoint, load_passage, quiz_items)
from reader_send.cli import main, parser
from reader_send.common import ReaderError

KEY = "assist-test-key-not-a-real-credential"
MODEL = "test-provider/study-model:1"
EXCERPT = "  Light bends toward the normal when entering a denser medium.\n屈折\t😀  "


def native(**changes):
    return {"version": 1, "id": 7, "kind": 1, "sourcePath": "/books/optics.epub",
            "sourceTitle": "Optics", "chapterTitle": "Refraction", "chapterHref": "text/ch2.xhtml",
            "xpath": "/html/body/p[4]", "si": 2, "pc": 20, "pp": 3, "vo": 0,
            "percentage": 0.25, "pageContext": False, "createdAt": 1793318400,
            "excerpt": EXCERPT, "note": "  Why does its direction change?\n", "unknown": {"preserve": True},
            **changes}


def response(content):
    return json.dumps({"choices": [{"message": {"role": "assistant", "content": content},
                                   "finish_reason": "stop"}]}).encode()


@contextmanager
def mock_api(reply):
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            raw = self.rfile.read(int(self.headers["Content-Length"]))
            requests.append({"path": self.path, "headers": dict(self.headers), "raw": raw,
                             "body": json.loads(raw)})
            status, headers, body = reply(requests[-1])
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            for key, value in headers.items():
                self.send_header(key, value)
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    server = HTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}/v1", requests
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


class AssistTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "annotation.json"
        self.source.write_text(json.dumps(native(), ensure_ascii=False), encoding="utf-8")
        self.environment = patch.dict(os.environ, {"READER_ASSIST_BASE_URL": "https://example.com/v1",
                                                  "READER_ASSIST_MODEL": MODEL,
                                                  "READER_ASSIST_API_KEY": KEY}, clear=True)
        self.environment.start()
        self.addCleanup(self.environment.stop)
        self.platform = patch("reader_send.assist.sys.platform", "darwin")
        self.platform.start()
        self.addCleanup(self.platform.stop)

    def invoke(self, mode="explain", *extra, source=None, output=None):
        stdout, stderr = io.StringIO(), io.StringIO()
        args = ["--cache-dir", str(self.root / "unused-cache"), "assist", mode, str(source or self.source),
                "--output", str(output or self.root / f"{mode}.md"), *extra]
        with patch("sys.stdout", stdout), patch("sys.stderr", stderr):
            status = main(args)
        self.assertNotIn(KEY, stdout.getvalue() + stderr.getvalue())
        return status, stdout.getvalue(), stderr.getvalue()

    def test_actual_http_explain_example_quiz_and_exact_provenance(self):
        answers = {}

        def reply(request):
            payload = request["body"]
            supplied = json.loads(payload["messages"][1]["content"])
            prompt = payload["messages"][0]["content"]
            if "actual study questions" in prompt:
                mode = "quiz"
                content = json.dumps({"questions": [
                    {"question": "Toward what does the light bend?", "answer": "Toward the normal."},
                    {"question": "What change of medium is described?", "answer": "Entry into a denser medium."}],
                    "limitations": "The supplied passage does not explain the microscopic mechanism."})
            elif "hypothetical example illustrating" in prompt:
                mode = "example"
                content = "Hypothetical example: light entering a denser glass block bends toward the normal. This illustrates: " + supplied["excerpt"]
            else:
                mode = "explain"
                content = "The passage describes the direction of refraction, not its microscopic cause. Supplied text: " + supplied["excerpt"]
            answers[mode] = content
            return 200, {}, response(content)

        anchors = []
        with mock_api(reply) as (base, requests), patch.dict(os.environ, {"READER_ASSIST_BASE_URL": base}):
            for mode in ("explain", "example", "quiz"):
                with self.subTest(mode=mode):
                    output = self.root / f"{mode}.json"
                    status, stdout, stderr = self.invoke(mode, "--format", "json", output=output)
                    self.assertEqual((status, stderr), (0, ""))
                    artifact = json.loads(output.read_text(encoding="utf-8"))
                    self.assertEqual(artifact["answer"], answers[mode])
                    self.assertEqual(artifact["excerpt"], EXCERPT)
                    self.assertEqual(artifact["source_record"], native())
                    self.assertEqual(artifact["source_anchor"]["native"]["vo"], 0)
                    self.assertEqual(artifact["source_anchor"]["native"]["unknown"], {"preserve": True})
                    self.assertEqual(artifact["source_id"], "/books/optics.epub")
                    self.assertEqual(artifact["kind"], "question")
                    self.assertEqual((artifact["mode"], artifact["model"]), (mode, MODEL))
                    self.assertRegex(artifact["generated_at"], r"^\d{4}-\d\d-\d\dT.*Z$")
                    self.assertFalse(artifact["native_writeback"])
                    self.assertEqual(json.loads(stdout)["anchor"], artifact["anchor"])
                    self.assertNotIn(KEY, output.read_text())
                    anchors.append(artifact["anchor"])
            self.assertEqual(len(requests), 3)
            for request in requests:
                self.assertEqual(request["path"], "/v1/chat/completions")
                self.assertEqual(request["headers"]["Authorization"], "Bearer " + KEY)
                self.assertEqual(request["headers"]["Content-Type"], "application/json")
                self.assertNotIn("Cookie", request["headers"])
                self.assertNotIn(KEY.encode(), request["raw"])
                self.assertEqual(request["body"]["model"], MODEL)
                self.assertEqual(request["body"]["max_tokens"], 2048)
                self.assertIs(request["body"]["stream"], False)
                supplied = json.loads(request["body"]["messages"][1]["content"])
                self.assertEqual(supplied["excerpt"], EXCERPT)
                self.assertEqual(supplied["note"], native()["note"])
                self.assertEqual(supplied["source_metadata"]["chapterTitle"], "Refraction")
                self.assertNotIn("unknown", supplied["source_metadata"])
                self.assertIn("Do not\ninvent book facts", request["body"]["messages"][0]["content"])
        self.assertEqual(len(set(anchors)), 1)
        self.assertFalse((self.root / "unused-cache").exists())
        self.assertEqual(json.loads(self.source.read_text()), native())

    def test_default_markdown_contains_answer_and_machine_readable_provenance(self):
        answer = "A specific explanation from the mock model, not a built-in answer."
        with mock_api(lambda request: (200, {}, response(answer))) as (base, requests), patch.dict(os.environ, {"READER_ASSIST_BASE_URL": base}):
            status, _, error = self.invoke()
        self.assertEqual((status, error), (0, ""))
        output = (self.root / "explain.md").read_text()
        self.assertIn(answer, output)
        self.assertIn(EXCERPT, output)
        self.assertIn("## Import provenance (JSON)", output)
        self.assertIn('"vo": 0', output)
        self.assertIn('"kind": "question"', output)
        self.assertIn('"source_id": "/books/optics.epub"', output)
        self.assertEqual(len(requests), 1)

    def test_quiz_markdown_has_actual_questions_and_answer_key(self):
        quiz = {"questions": [{"question": "Where does light bend?", "answer": "Toward the normal."}],
                "limitations": "The passage does not give refractive indices."}
        with mock_api(lambda request: (200, {}, response(json.dumps(quiz)))) as (base, _), patch.dict(os.environ, {"READER_ASSIST_BASE_URL": base}):
            status, _, error = self.invoke("quiz")
        self.assertEqual((status, error), (0, ""))
        output = (self.root / "quiz.md").read_text()
        for expected in ("### Questions", "Where does light bend?", "### Answer key", "Toward the normal.", "### Limitations"):
            self.assertIn(expected, output)

    def test_dry_run_needs_no_key_makes_no_network_or_output_writes(self):
        del os.environ["READER_ASSIST_API_KEY"]
        with patch("reader_send.assist.http.client.HTTPConnection", side_effect=AssertionError("network")), \
                patch("reader_send.assist.http.client.HTTPSConnection", side_effect=AssertionError("network")), \
                patch("reader_send.assist.atomic_write", side_effect=AssertionError("write")):
            status, stdout, error = self.invoke("quiz", "--dry-run", output=self.root / "new-dir/quiz.md")
        self.assertEqual((status, error), (0, ""))
        self.assertTrue(json.loads(stdout)["dry_run"])
        self.assertEqual(json.loads(stdout)["passage_characters"], len(EXCERPT))
        self.assertEqual(list(self.root.iterdir()), [self.source])

    def test_env_only_configuration_validation_and_no_secret_arguments(self):
        for bad in ("", "http://example.com/v1", "http://127.1/v1", "http://localhost.evil/v1",
                    "https://u:secret@example.com/v1", "https://example.com/v1?key=secret",
                    "https://example.com/v1#secret", "https://example.com:0/v1", "https://example.com:65536/v1",
                    "https://example.com\n/v1", "https://example.com\\@evil/v1", "https://example.com/%2e%2e", "https://example.com/../v1"):
            with self.subTest(url=bad), self.assertRaises(ReaderError):
                endpoint(bad)
        self.assertEqual(endpoint("https://EXAMPLE.com/v1/"), "https://example.com/v1/chat/completions")
        self.assertEqual(endpoint("https://example.com/v1/chat/completions"), "https://example.com/v1/chat/completions")
        self.assertEqual(endpoint("http://localhost:1234/v1"), "http://localhost:1234/v1/chat/completions")
        for bad in ("", "model\nsecret", "model name", "a" * 201, "你好", "?model"):
            with self.subTest(model=bad), patch.dict(os.environ, {"READER_ASSIST_MODEL": bad}), self.assertRaises(ReaderError):
                configuration()
        for bad in ("", "has space", "line\nbreak", "x" * 4097, "你好"):
            with self.subTest(key=bad), patch.dict(os.environ, {"READER_ASSIST_API_KEY": bad}), self.assertRaises(ReaderError):
                configuration()
        del os.environ["READER_ASSIST_API_KEY"]
        with patch.dict(os.environ, {"OPENAI_API_KEY": KEY, "READER_SYNC_KEY": KEY}), self.assertRaises(ReaderError):
            configuration()
        with patch("sys.stderr", io.StringIO()), self.assertRaises(SystemExit):
            parser().parse_args(["assist", "explain", str(self.source), "--api-key", "not-supported"])
        with patch("reader_send.assist.sys.platform", "linux"):
            status, _, error = self.invoke("explain", "--dry-run")
        self.assertEqual(status, 2)
        self.assertIn("Mac-only", error)

    def test_https_uses_mandatory_certificate_verification(self):
        seen = {}

        class Response:
            status = 200
            stream = io.BytesIO(response("Grounded explanation"))
            def getheader(self, name, default=None):
                return {"Content-Type": "application/json"}.get(name, default)
            def read1(self, size):
                return self.stream.read(size)

        class Connection:
            def __init__(self, host, port, **kwargs):
                seen.update(host=host, port=port, **kwargs)
            def request(self, method, route, **kwargs):
                seen.update(method=method, route=route, **kwargs)
            def getresponse(self):
                return Response()
            def close(self):
                seen["closed"] = True

        with patch("reader_send.assist.http.client.HTTPSConnection", Connection):
            self.assertEqual(complete("https://example.com/v1/chat/completions", KEY, b"{}", 10), "Grounded explanation")
        self.assertTrue(seen["context"].check_hostname)
        self.assertEqual(seen["context"].verify_mode, ssl.CERT_REQUIRED)
        self.assertTrue(seen["closed"])
        self.assertEqual((seen["host"], seen["port"], seen["route"]), ("example.com", 443, "/v1/chat/completions"))

    def test_redirect_and_error_bodies_are_never_followed_or_echoed(self):
        for status_code in (302, 401, 429, 500):
            with self.subTest(status=status_code), mock_api(lambda request: (status_code, {"Location": "http://localhost:1/stolen?" + KEY}, KEY.encode())) as (base, requests), \
                    patch.dict(os.environ, {"READER_ASSIST_BASE_URL": base}):
                status, stdout, error = self.invoke()
                self.assertEqual(status, 2)
                self.assertEqual(stdout, "")
                self.assertIn("redirect refused" if status_code == 302 else f"HTTP {status_code}", error)
                self.assertEqual(len(requests), 1)
                self.assertFalse((self.root / "explain.md").exists())

    def test_secret_reflection_and_secret_in_source_are_rejected_without_artifacts(self):
        with mock_api(lambda request: (200, {}, response("reflected " + KEY))) as (base, requests), patch.dict(os.environ, {"READER_ASSIST_BASE_URL": base}):
            status, _, error = self.invoke()
        self.assertEqual(status, 2)
        self.assertIn("credential material", error)
        self.assertFalse((self.root / "explain.md").exists())
        self.source.write_text(json.dumps(native(note=KEY)))
        with patch("reader_send.assist.complete", side_effect=AssertionError("network")):
            status, _, error = self.invoke()
        self.assertEqual(status, 2)
        self.assertIn("credential material", error)
        escaped_key = 'test-key-with-"quote-and-\\slash'
        self.source.write_text(json.dumps(native(note=escaped_key)))
        with patch.dict(os.environ, {"READER_ASSIST_API_KEY": escaped_key}), patch("reader_send.assist.complete", side_effect=AssertionError("network")):
            status, stdout, error = self.invoke("explain", "--format", "json")
        self.assertEqual(status, 2)
        self.assertNotIn(escaped_key, stdout + error)
        self.assertIn("credential material", error)

    def test_malformed_oversized_deep_and_truncated_responses_fail_closed(self):
        malformed = (b"not json " + KEY.encode(), b"[]", b'{"choices":[]}',
                     response(""), b'{"choices":[],"choices":[]}',
                     b"[" * 33 + b"0" + b"]" * 33,
                     response("x" * (MAX_RESPONSE_BYTES + 1)),
                     json.dumps({"choices": [{"message": {"content": "cut short"}, "finish_reason": "length"}]}).encode(),
                     json.dumps({"choices": [{"message": {"content": None}, "finish_reason": "stop"}]}).encode())
        for raw in malformed:
            with self.subTest(size=len(raw)), mock_api(lambda request: (200, {}, raw)) as (base, _), patch.dict(os.environ, {"READER_ASSIST_BASE_URL": base}):
                status, _, error = self.invoke()
                self.assertEqual(status, 2)
                self.assertIn("Assist", error)
                self.assertFalse((self.root / "explain.md").exists())
        for content in ('{"questions":[]}', '{"questions":[{"question":"Where?"}],"limitations":"Unknown"}',
                        '{"questions":[{"question":"Where?","answer":""}],"limitations":"Unknown"}', 'not JSON'):
            with self.subTest(content=content), self.assertRaises(ReaderError):
                quiz_items(content)

    def test_selected_json_bounds_anchors_and_unavailable_metadata(self):
        first = load_passage(self.source)
        self.source.write_text(json.dumps(native(note="Edited note")))
        self.assertEqual(first["anchor"], load_passage(self.source)["anchor"])
        no_offset = native()
        del no_offset["vo"]
        self.source.write_text(json.dumps([native(), no_offset]))
        with self.assertRaisesRegex(ReaderError, "exactly one"):
            load_passage(self.source)
        with self.assertRaisesRegex(ReaderError, "exactly one"):
            load_passage(self.source, "7")
        no_offset["id"] = 8
        self.source.write_text(json.dumps({"annotations": [native(), no_offset]}))
        selected = load_passage(self.source, "8")
        self.assertNotIn("vo", selected["source_anchor"]["native"])
        self.assertNotEqual(first["anchor"], selected["anchor"])
        other = native(sourcePath="/books/another.epub")
        self.source.write_text(json.dumps([native(), other]))
        with self.assertRaisesRegex(ReaderError, "exactly one"):
            load_passage(self.source, "7")
        self.assertEqual(load_passage(self.source, "7", "/books/another.epub")["source_record"], other)
        for changes in ({"excerpt": "x" * (MAX_PASSAGE + 1)}, {"note": "x" * (MAX_NOTE + 1)}, {"excerpt": ""}):
            self.source.write_text(json.dumps(native(**changes)))
            with self.assertRaises(ReaderError):
                load_passage(self.source)
        original = {"id": "external-id", "book_path": "/source.epub", "source_anchor": ' {"xpath":"/p","vo":0} ',
                    "selection": EXCERPT, "note": "", "kind": "ReviewLater"}
        self.source.write_text(json.dumps(original))
        selected = load_passage(self.source)
        self.assertEqual(selected["source_anchor"], original["source_anchor"])
        self.assertEqual(selected["source_metadata"], {})
        self.assertEqual(selected["kind"], "review_later")
        self.assertEqual(selected["source_record"], original)

    def test_single_entry_native_markdown_preserves_export_and_location(self):
        markdown = self.root / "export.md"
        raw = """# Optics

- Source: /books/optics\\.epub

## 7

- Title: Optics
- Source: /books/optics\\.epub
- Type: Review later
- Chapter: Refraction
- Chapter path: ch2\\.xhtml
- Spine index: 2
- Progress: 25\\.00%
- XPath: /html/body/p\\[4\\]
- Visible text offset: 0
- Page at capture: 4/20
- Created (Unix UTC): 1793318400
- Text kind: Selected excerpt

>   Light &lt; normal &amp; medium\\.  

**Note:** Why?
"""
        markdown.write_text(raw, encoding="utf-8")
        selected = load_passage(markdown)
        original_anchor = selected["anchor"]
        self.assertEqual(selected["source_id"], "/books/optics.epub")
        self.assertEqual(selected["excerpt"], "  Light < normal & medium.  ")
        self.assertEqual(selected["source_record"]["export_markdown"], raw)
        self.assertEqual(selected["source_anchor"]["fields"]["XPath"], "/html/body/p[4]")
        self.assertEqual(selected["source_anchor"]["fields"]["Visible text offset"], "0")
        self.assertEqual(selected["kind"], "review_later")
        with mock_api(lambda request: (200, {}, response("Example grounded in the exported excerpt."))) as (base, _), patch.dict(os.environ, {"READER_ASSIST_BASE_URL": base}):
            status, _, error = self.invoke("example", source=markdown)
        self.assertEqual((status, error), (0, ""))
        entry = raw[raw.index("## 7"):]
        markdown.write_text(raw + "\n" + entry.replace("## 7", "## 8", 1))
        with self.assertRaisesRegex(ReaderError, "exactly one"):
            load_passage(markdown)
        self.assertEqual(load_passage(markdown, "8")["source_record"]["id"], "8")
        all_inbox = "# Reading inbox\n\n- Scope: All sources\n\n" + entry + "\n" + entry.replace("/books/optics", "/books/second").replace("Light", "Unselected text")
        markdown.write_text(all_inbox)
        with self.assertRaisesRegex(ReaderError, "exactly one"):
            load_passage(markdown, "7")
        selected = load_passage(markdown, "7", "/books/optics.epub")
        self.assertEqual(selected["anchor"], original_anchor)
        self.assertNotIn("Unselected text", selected["source_record"]["export_markdown"])
        self.assertEqual(selected["source_anchor"]["fields"]["Visible text offset"], "0")
        with mock_api(lambda request: (200, {}, response("Grounded explanation."))) as (base, requests), patch.dict(os.environ, {"READER_ASSIST_BASE_URL": base}):
            status, _, error = self.invoke("explain", "--entry-id", "7", "--source-path", "/books/optics.epub", source=markdown)
        self.assertEqual((status, error), (0, ""))
        self.assertNotIn(b"Unselected text", requests[0]["raw"])

    def test_no_source_overwrite_and_existing_output_fails_before_network(self):
        with patch("reader_send.assist.complete", side_effect=AssertionError("network")):
            status, _, error = self.invoke("explain", "--force", output=self.source)
            self.assertEqual(status, 2)
            self.assertIn("must not overwrite", error)
            destination = self.root / "explain.md"
            destination.write_text("existing")
            status, _, error = self.invoke()
            self.assertEqual(status, 2)
            self.assertIn("output exists", error)
            self.assertEqual(destination.read_text(), "existing")


if __name__ == "__main__":
    unittest.main()
