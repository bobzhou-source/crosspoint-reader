from __future__ import annotations

import json
import unittest
from unittest.mock import patch

from reader_send import x_browser as xb
from reader_send.common import ReaderError
from reader_send.x_adapter import thread_item

REQUESTED = "102"


def tweet(ident, author, text, *, conversation=None, reply_to=None, name=None, screen=None,
          note=None, media=(), quote=None, created="Fri Oct 30 21:07:02 +0000 2026"):
    legacy = {"conversation_id_str": str(conversation or ident), "created_at": created,
              "full_text": text, "user_id_str": str(author), "entities": {}, "id_str": str(ident)}
    if reply_to:
        legacy["in_reply_to_status_id_str"] = str(reply_to)
    if media:
        legacy["extended_entities"] = {"media": list(media)}
    result = {"__typename": "Tweet", "rest_id": str(ident), "legacy": legacy,
              "core": {"user_results": {"result": {
                  "__typename": "User", "rest_id": str(author),
                  "core": {"name": name or f"Author {author}", "screen_name": screen or f"user{author}"}}}}}
    if note:
        result["note_tweet"] = {"note_tweet_results": {"result": {"text": note, "entity_set": {"urls": []}}}}
    if quote:
        result["quoted_status_result"] = {"result": quote}
    return result


PHOTO = {"media_key": "3_photo1", "type": "photo", "media_url_https": "https://pbs.twimg.com/media/photo1.jpg"}


def page(*tweets, cursor=None):
    entries = [{"entryId": f"tweet-{item['rest_id']}", "content": {
        "__typename": "TimelineTimelineItem", "entryType": "TimelineTimelineItem",
        "itemContent": {"itemType": "TimelineTweet",
                        "tweet_results": {"result": item}}}} for item in tweets]
    if cursor:
        entries.append({"entryId": f"cursor-showmorethreads-{cursor}", "content": {
            "__typename": "TimelineTimelineCursor", "cursorType": "ShowMoreThreads", "value": cursor}})
    return {"data": {"threaded_conversation_with_injections_v2": {"instructions": [
        {"type": "TimelineClearCache"},
        {"type": "TimelineAddEntries", "entries": entries},
    ]}}}


def thread_page(*, cursor=None, module=False):
    quoted = tweet(400, "9", "Quoted context", name="Other", screen="other")
    root = tweet(100, "1", "Root post", screen="writer")
    middle = tweet(101, "1", "Second post", conversation="100", reply_to="100", screen="writer",
                   media=[PHOTO], quote=quoted)
    last = tweet(102, "1", "Third post", conversation="100", reply_to="101", screen="writer",
                 note="Third post with a long note body")
    stranger = tweet(200, "2", "unrelated reply", conversation="100", reply_to="100",
                     name="Stranger", screen="stranger")
    elsewhere = tweet(300, "1", "related tweet in another conversation", screen="writer")
    payload = page(root, middle, last, stranger, elsewhere, cursor=cursor)
    if module:
        module_entry = {"entryId": "conversationthread-100", "content": {
            "__typename": "TimelineTimelineModule", "entryType": "TimelineTimelineModule",
            "items": [{"entryId": "m-101", "item": {"itemContent": {
                "itemType": "TimelineTweet", "tweet_results": {"result": middle}}}}]}}
        payload["data"]["threaded_conversation_with_injections_v2"]["instructions"][1]["entries"].append(module_entry)
    return payload


class EndpointTest(unittest.TestCase):
    def test_loopback_only_and_bare_origin(self):
        self.assertEqual(xb.cdp_endpoint(), "http://127.0.0.1:9333")
        self.assertEqual(xb.cdp_endpoint("http://localhost:9333/"), "http://localhost:9333")
        self.assertEqual(xb.cdp_endpoint("http://[::1]:9333"), "http://[::1]:9333")
        for value in ("https://127.0.0.1:9333", "http://192.168.1.20:9333", "http://example.com:9333",
                      "http://127.0.0.1:9333/json", "http://127.0.0.1", "http://u:p@127.0.0.1:9333"):
            with self.subTest(value=value), self.assertRaises(ReaderError):
                xb.cdp_endpoint(value)

    def test_capture_rejects_source_and_endpoint_before_network(self):
        with patch.object(xb, "_devtools", side_effect=AssertionError("network used")):
            with self.assertRaisesRegex(ReaderError, "x.com/twitter.com"):
                xb.capture_x_url("https://example.com/writer/status/102", endpoint="http://127.0.0.1:9333")
            with self.assertRaisesRegex(ReaderError, "loopback"):
                xb.capture_x_url("https://x.com/writer/status/102", endpoint="http://10.0.0.5:9333")


class NormalizationTest(unittest.TestCase):
    def test_author_thread_only_keeps_connected_author_posts(self):
        payload = xb.api_payload([thread_page()], REQUESTED)
        self.assertEqual(payload["data"][0]["id"], REQUESTED)
        ids = [item["id"] for item in payload["includes"]["tweets"]]
        self.assertEqual(ids, ["100", "101", "400"])
        authors = {user["id"] for user in payload["includes"]["users"]}
        self.assertEqual(authors, {"1", "9"})
        self.assertEqual(payload["includes"]["media"][0]["media_key"], "3_photo1")
        self.assertEqual(payload["includes"]["media"][0]["url"], "https://pbs.twimg.com/media/photo1.jpg")
        self.assertEqual(payload["data"][0]["created_at"], "2026-10-30T21:07:02Z")
        self.assertEqual(payload["data"][0]["note_tweet"]["text"], "Third post with a long note body")
        self.assertNotIn("_warnings", payload)

    def test_rendered_thread_becomes_an_epub_without_other_peoples_replies(self):
        payload = xb.api_payload([thread_page()], REQUESTED)
        item = thread_item("https://x.com/writer/status/102", payload)
        self.assertEqual(item.provenance["post_ids"], "100,101,102")
        self.assertEqual(item.author, "Author 1 (@writer)")
        for text in ("Root post", "Second post", "Third post with a long note body", "Quoted context"):
            self.assertIn(text, item.content)
        for text in ("unrelated reply", "related tweet in another conversation", "Stranger"):
            self.assertNotIn(text, item.content)

    def test_partial_page_warns_instead_of_claiming_completeness(self):
        payload = xb.api_payload([thread_page(cursor="12345")], REQUESTED)
        warnings = payload["_warnings"]
        self.assertEqual(len(warnings), 1)
        self.assertIn("first batch", warnings[0])
        self.assertIn("may stop early", warnings[0])

    def test_module_entries_and_missing_post(self):
        payload = xb.api_payload([thread_page(module=True)], REQUESTED)
        self.assertEqual(payload["data"][0]["id"], REQUESTED)
        self.assertIsNone(xb.api_payload([thread_page()], "999"))
        # A reply by someone else is still readable when that reply itself is requested.
        reply = xb.api_payload([thread_page()], "200")
        self.assertEqual([item["id"] for item in reply["includes"]["tweets"]], [])

    def test_post_without_author_fails_loudly(self):
        broken = thread_page()
        target = broken["data"]["threaded_conversation_with_injections_v2"]["instructions"][1]["entries"][2]
        target["content"]["itemContent"]["tweet_results"]["result"]["legacy"].pop("user_id_str")
        target["content"]["itemContent"]["tweet_results"]["result"]["core"] = {}
        with self.assertRaisesRegex(ReaderError, "without an author"):
            xb.api_payload([broken], REQUESTED)


class RenderedPageTest(unittest.TestCase):
    def test_rendered_article_becomes_a_single_post_with_a_warning(self):
        dom = {"signedIn": True, "loginPrompt": False, "bodyText": "", "articles": [
            {"id": "102", "name": "Writer  @writer", "text": "Rendered text",
             "datetime": "2026-10-30T08:35:29.000Z",
             "photos": ["https://pbs.twimg.com/media/photo1?format=jpg&name=small"]}]}
        payload = xb._payload_from_dom(dom, "102")
        self.assertEqual(payload["data"][0]["author_id"], "writer")
        self.assertEqual(payload["data"][0]["text"], "Rendered text")
        self.assertEqual(payload["data"][0]["created_at"], "2026-10-30T08:35:29.000Z")
        self.assertTrue(payload["includes"]["media"][0]["url"].startswith("https://pbs.twimg.com/media/photo1"))
        self.assertIn("may omit the rest of the thread", payload["_warnings"][0])
        item = thread_item("https://x.com/writer/status/102", payload)
        self.assertEqual(item.provenance["post_ids"], "102")

    def test_unrelated_articles_are_not_borrowed(self):
        dom = {"articles": [{"id": "999", "name": "Other @other", "text": "quotes 102",
                             "datetime": "", "photos": []}]}
        self.assertIsNone(xb._payload_from_dom(dom, "102"))
        self.assertIsNone(xb._payload_from_dom({"articles": [{"id": "102", "text": "  "}]}, "102"))


class CaptureFlowTest(unittest.TestCase):
    DETAIL = "https://x.com/i/api/graphql/qpemjmVMoTWDvcRT43JIVQ/TweetDetail?variables="

    class FakeSession:
        def __init__(self, script):
            self.script = list(script)
            self.calls: list[str] = []
            self.closed = False

        def call(self, method, params, deadline):
            self.calls.append(method)
            if not self.script:
                return {}
            step = self.script.pop(0)
            if isinstance(step, Exception):
                raise step
            return step

        def next_event(self, deadline):
            if not self.script:
                raise xb._Timeout
            step = self.script.pop(0)
            if isinstance(step, Exception):
                raise step
            return step

        def close(self):
            self.closed = True

    def run_capture(self, script, *, dom=None):
        session = self.FakeSession(script)
        closed: list[str] = []
        with patch.object(xb, "_devtools", return_value={"Browser": "Chrome/156"}):
            with patch.object(xb, "_new_target", return_value={"id": "OWNED-TAB",
                                                               "webSocketDebuggerUrl": "ws://127.0.0.1:9333/x"}):
                with patch.object(xb, "_Session", lambda *args: session):
                    with patch.object(xb, "_close_target", side_effect=lambda base, ident, timeout: closed.append(ident)):
                        with patch.object(xb, "_dom_snapshot", return_value=dom or {}):
                            payload = xb.capture_x_url("https://x.com/writer/status/102")
        return payload, session, closed

    def script(self, *, focal="102", body=None):
        variables = json.dumps({"focalTweetId": focal})
        return [
            {}, {},
            {"frameId": "F1"},
            {"method": "Network.responseReceived", "params": {
                "requestId": "r1", "response": {"url": self.DETAIL + variables}}},
            {"method": "Network.loadingFinished", "params": {"requestId": "r1"}},
            {"body": json.dumps(body if body is not None else thread_page(cursor="777"))},
        ]

    def test_capture_uses_own_tab_and_closes_only_it(self):
        payload, session, closed = self.run_capture(self.script())
        self.assertEqual(payload["data"][0]["id"], "102")
        self.assertIn("first batch", payload["_warnings"][0])
        self.assertEqual(closed, ["OWNED-TAB"])
        self.assertTrue(session.closed)
        self.assertIn("Network.getResponseBody", session.calls)

    def test_other_focal_response_is_ignored(self):
        detail = self.DETAIL + json.dumps({"focalTweetId": "999"})
        script = [{}, {}, {"frameId": "F1"},
                  {"method": "Network.responseReceived", "params": {
                      "requestId": "r1", "response": {"url": detail}}},
                  {"method": "Network.loadingFinished", "params": {"requestId": "r1"}},
                  xb._Timeout()]
        session = self.FakeSession(script)
        closed: list[str] = []
        with patch.object(xb, "_devtools", return_value={"Browser": "Chrome/156"}), \
                patch.object(xb, "_new_target", return_value={"id": "OWNED-TAB", "webSocketDebuggerUrl": "ws://x"}), \
                patch.object(xb, "_Session", lambda *args: session), \
                patch.object(xb, "_close_target", side_effect=lambda base, ident, timeout: closed.append(ident)), \
                patch.object(xb, "_dom_snapshot", return_value={}):
            with self.assertRaisesRegex(ReaderError, "no post data"):
                xb.capture_x_url("https://x.com/writer/status/102")
        self.assertEqual(closed, ["OWNED-TAB"])
        self.assertNotIn("Network.getResponseBody", session.calls)

    def test_login_and_missing_post_messages_are_actionable(self):
        login = xb._failure("https://x.com/writer/status/102", "102",
                            "https://x.com/i/flow/login", [], {"loginPrompt": True}, 30)
        self.assertIn("Sign in", str(login))
        signed_out = xb._failure("https://x.com/writer/status/102", "102",
                                 "https://x.com/writer/status/102", [], {"signedIn": False}, 30)
        self.assertIn("no signed-in X session", str(signed_out))
        gone = xb._failure("https://x.com/writer/status/102", "102",
                           "https://x.com/writer/status/102", [{}],
                           {"signedIn": True, "bodyText": "This post is unavailable."}, 30)
        self.assertIn("unavailable", str(gone))
        empty = xb._failure("https://x.com/writer/status/102", "102",
                            "https://x.com/writer/status/102", [], {"signedIn": True, "bodyText": ""}, 30)
        self.assertIn("no post data", str(empty))

    def test_following_signed_out_onboarding_redirect_is_a_sign_in_error_not_a_timeout_crash(self):
        onboarding = "https://x.com/i/jf/onboarding/web?redirect_after_login=%2Fhome&mode=login"
        script = [{}, {}, {"frameId": "F1"},
                  {"method": "Page.frameNavigated", "params": {"frame": {"url": onboarding}}},
                  xb._Timeout(), xb._Timeout()]
        session = self.FakeSession(script)
        closed: list[str] = []
        with patch.object(xb, "_devtools", return_value={"Browser": "Chrome/156"}), \
                patch.object(xb, "_new_target", return_value={"id": "OWNED-TAB", "webSocketDebuggerUrl": "ws://x"}), \
                patch.object(xb, "_Session", lambda *args: session), \
                patch.object(xb, "_close_target", side_effect=lambda base, ident, timeout: closed.append(ident)):
            with self.assertRaisesRegex(ReaderError, "sign-in page"):
                xb.capture_following_feed()
        self.assertEqual(closed, ["OWNED-TAB"])
        self.assertTrue(session.closed)

    def test_rendered_empty_entry_is_reported_as_missing(self):
        placeholder = {"data": {"threaded_conversation_with_injections_v2": {"instructions": [
            {"type": "TimelineAddEntries", "entries": [
                {"entryId": "tweet-102", "content": {"__typename": "TimelineTimelineItem",
                 "itemContent": {"itemType": "TimelineTweet", "tweet_results": {}}}}]}]}}}
        error = xb._failure("https://x.com/writer/status/102", "102",
                            "https://x.com/writer/status/102", [placeholder],
                            {"signedIn": True, "bodyText": "", "articles": []}, 30)
        self.assertIn("deleted, never existed", str(error))


if __name__ == "__main__":
    unittest.main()
