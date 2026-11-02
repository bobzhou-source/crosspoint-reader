from __future__ import annotations

import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest.mock import patch

from reader_send import x_following
from reader_send.cache import Cache
from reader_send.common import ReaderError
from reader_send.network import PublicHTTP
from reader_send.x_browser import feed_entries

NOW = datetime.now(timezone.utc).replace(microsecond=0)


def stamp(when: datetime) -> str:
    return when.strftime("%a %b %d %H:%M:%S %z %Y")


def raw_tweet(ident, *, author="1", text=None, when=None, screen=None, name=None,
              reply_to=None, conversation=None, quote=None, retweeted=None):
    legacy = {"conversation_id_str": str(conversation or ident), "created_at": stamp(when or NOW),
              "full_text": text or f"post text {ident}", "user_id_str": str(author),
              "entities": {}, "id_str": str(ident)}
    if reply_to:
        legacy["in_reply_to_status_id_str"] = str(reply_to)
    if retweeted:
        legacy["retweeted_status_result"] = {"result": retweeted}
    result = {"rest_id": str(ident), "legacy": legacy,
              "core": {"user_results": {"result": {"rest_id": str(author),
                        "core": {"name": name or f"Author {author}", "screen_name": screen or f"user{author}"}}}}}
    if quote:
        result["quoted_status_result"] = {"result": quote}
    return result


def tweet_entry(raw, *, promoted=False, component=None, entry_id=None):
    content = {"__typename": "TimelineTimelineItem",
               "itemContent": {"itemType": "TimelineTweet", "tweet_results": {"result": raw}}}
    if promoted:
        content["clientEventInfo"] = {"component": component or "ranked_following_promoted"}
    return {"entryId": entry_id or (f"promoted-tweet-{raw['rest_id']}-1" if promoted else f"tweet-{raw['rest_id']}"),
            "content": content}


def module_entry(*raws, ident="900"):
    return {"entryId": f"home-conversation-{ident}", "content": {
        "__typename": "TimelineTimelineModule", "displayType": "VerticalConversation",
        "items": [{"item": {"itemContent": {"itemType": "TimelineTweet",
                                            "tweet_results": {"result": raw}}}} for raw in raws]}}


def cursor(kind):
    return {"entryId": f"cursor-{kind}-1", "content": {"__typename": "TimelineTimelineCursor",
                                                       "cursorType": kind, "value": "1"}}


def home_page(*entries):
    return {"data": {"home": {"home_timeline_urt": {"instructions": [
        {"type": "TimelineAddEntries", "entries": list(entries)}]}}}}


class FeedEntryTest(unittest.TestCase):
    def test_ads_reposts_modules_and_cursors(self):
        original = raw_tweet("500", author="5", when=NOW - timedelta(hours=40), screen="orig")
        repost = raw_tweet("300", author="7", when=NOW - timedelta(hours=2), screen="booster",
                           text="RT @orig: old post", retweeted=original)
        page = home_page(
            cursor("Top"),
            tweet_entry(raw_tweet("100", when=NOW - timedelta(hours=1))),
            tweet_entry(raw_tweet("200", when=NOW - timedelta(days=60)), promoted=True),
            tweet_entry(raw_tweet("201"), promoted=True, component="ranked_following_promoted",
                        entry_id="tweet-201"),
            tweet_entry(repost),
            module_entry(raw_tweet("700", author="2", when=NOW - timedelta(hours=3), screen="parent"),
                         raw_tweet("701", author="1", reply_to="700", when=NOW - timedelta(hours=1))),
            cursor("Bottom"),
        )
        entries = feed_entries([page])
        self.assertEqual([entry["id"] for entry in entries], ["100", "200", "201", "500", "700", "701"])
        self.assertEqual([entry["promoted"] for entry in entries], [False, True, True, False, False, False])
        reposted = entries[3]
        self.assertEqual(reposted["reposted_by"]["username"], "booster")
        self.assertEqual(reposted["created_at"], NOW - timedelta(hours=2))
        self.assertEqual(reposted["post_created_at"], NOW - timedelta(hours=40))
        self.assertEqual(entries[0]["created_at"], NOW - timedelta(hours=1))


class SyncTest(unittest.TestCase):
    def setUp(self):
        self.workspace = tempfile.TemporaryDirectory()
        self.cache = Cache(Path(self.workspace.name) / "cache")
        self.client = PublicHTTP(1)

    def tearDown(self):
        self.workspace.cleanup()

    def run_sync(self, page, **kwargs):
        with patch("reader_send.x_following.capture_following_feed", return_value=[page]), \
                patch("reader_send.x_following.build_epub", return_value=b"epub-bytes"):
            return x_following.sync(self.cache, self.client, **kwargs)

    def page(self, *entries):
        return home_page(*entries)

    def test_window_seen_state_and_canonical_dedupe(self):
        original = raw_tweet("500", author="5", when=NOW - timedelta(hours=40), screen="orig")
        page = self.page(
            tweet_entry(raw_tweet("100", when=NOW - timedelta(hours=1))),
            tweet_entry(raw_tweet("101", when=NOW - timedelta(hours=30))),
            tweet_entry(raw_tweet("200", when=NOW - timedelta(days=90)), promoted=True),
            tweet_entry(raw_tweet("100", when=NOW - timedelta(hours=1))),
            tweet_entry(raw_tweet("300", author="7", when=NOW - timedelta(hours=2), screen="booster",
                                  text="RT @orig", retweeted=original)),
        )
        first = self.run_sync(page, since_hours=24, limit=50)
        self.assertEqual((first["collected"], first["created"], first["skipped_window"]), (5, 2, 1))
        self.assertEqual((first["skipped_ads"], first["skipped_duplicate"]), (1, 1))
        self.assertEqual([item["post_id"] for item in first["items"]], ["100", "500"])
        self.assertEqual(first["errors"], [])
        saved = x_following.load_state(self.cache)
        self.assertEqual(saved["seen"], ["100", "500"])
        self.assertTrue(saved["last_run"])
        boost = self.cache.get(first["items"][1]["id"])
        self.assertEqual(boost["provenance"]["reposted_by"], "Author 7 (@booster)")
        self.assertEqual(boost["provenance"]["feed"], "following")

        second = self.run_sync(page, since_hours=24, limit=50)
        self.assertEqual((second["created"], second["skipped_seen"]), (0, 3))
        self.assertEqual(second["items"], [])
        self.assertTrue(any("No posts newer than" in warning for warning in second["warnings"]))

        newer = self.page(tweet_entry(raw_tweet("102", when=NOW - timedelta(minutes=5))))
        third = self.run_sync(newer, since_hours=24, limit=50)
        self.assertEqual(third["created"], 1)
        self.assertEqual(x_following.load_state(self.cache)["seen"], ["102", "100", "500"])

    def test_limit_truncates_and_leaves_the_rest_for_a_later_pass(self):
        page = self.page(*[tweet_entry(raw_tweet(str(400 + offset)))
                           for offset in range(3)])
        first = self.run_sync(page, since_hours=24, limit=2)
        self.assertEqual(first["created"], 2)
        self.assertTrue(first["truncated"])
        self.assertTrue(any("--limit is 2" in warning for warning in first["warnings"]))
        second = self.run_sync(page, since_hours=24, limit=2)
        self.assertEqual(second["created"], 1)
        self.assertFalse(second["truncated"])

    def test_dry_run_stores_nothing_and_advances_nothing(self):
        page = self.page(tweet_entry(raw_tweet("600")))
        result = self.run_sync(page, since_hours=24, limit=50, dry_run=True)
        self.assertEqual(result["created"], 1)
        self.assertTrue(result["items"][0]["dry_run"])
        self.assertFalse(x_following.state_path(self.cache).exists())
        self.assertEqual(self.cache.entries(), [])

    def test_second_pass_is_refused_while_the_lock_is_held(self):
        with x_following.lock(self.cache):
            with self.assertRaisesRegex(ReaderError, "Another x-following sync"):
                self.run_sync(self.page(), since_hours=24, limit=50)

    def test_browser_failure_is_reported_without_touching_state(self):
        with patch("reader_send.x_following.capture_following_feed",
                   side_effect=ReaderError("Chrome is not running")):
            result = x_following.sync(self.cache, self.client, since_hours=24, limit=50)
        self.assertEqual(result["errors"][0]["stage"], "browser")
        self.assertIn("Chrome is not running", result["errors"][0]["error"])
        self.assertEqual(result["created"], 0)
        self.assertFalse(x_following.state_path(self.cache).exists())

    def test_timeline_items_are_single_posts_with_quotes_not_threads(self):
        quote = raw_tweet("800", author="9", screen="quoted", text="quoted context")
        root = raw_tweet("700", author="1", when=NOW - timedelta(hours=2), screen="writer",
                         quote=quote)
        reply = raw_tweet("701", author="1", conversation="700", reply_to="700",
                          when=NOW - timedelta(hours=1), screen="writer", text="second post")
        recorded = []

        def fake_epub(item, article_id, client, **kwargs):
            recorded.append(item)
            return b"epub-bytes"

        page = self.page(tweet_entry(root), tweet_entry(reply))
        with patch("reader_send.x_following.capture_following_feed", return_value=[page]), \
                patch("reader_send.x_following.build_epub", side_effect=fake_epub):
            result = x_following.sync(self.cache, self.client, since_hours=24, limit=50)
        self.assertEqual([item["post_id"] for item in result["items"]], ["700", "701"])
        self.assertEqual(len(recorded), 2)
        for item in recorded:
            self.assertNotIn("Thread includes", " ".join(item.warnings))
            self.assertEqual(len(item.provenance["post_ids"].split(",")), 1)
        texts = [item.content for item in recorded]
        self.assertIn("quoted context", texts[0])
        self.assertNotIn("second post", texts[0])
        self.assertNotIn("post text 700", texts[1])


if __name__ == "__main__":
    unittest.main()
