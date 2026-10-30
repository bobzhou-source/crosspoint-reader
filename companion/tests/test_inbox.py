import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from reader_send.cache import Cache
from reader_send.cli import main
from reader_send.common import ReaderError
from reader_send.epub import build_epub
from reader_send.extract import Article, extract
from reader_send.model import ReadingItem, source_id
from reader_send.network import PublicHTTP, Response
from reader_send.subscriptions import add, feed_entries, sync
from reader_send.x_adapter import api_thread, load_export, post_id, thread_item


FEED = b'''<rss version="2.0"><channel><title>Selected publication</title>
<item><guid>post-two</guid><title>Second post</title><link>https://example.com/two</link></item>
<item><guid>post-one</guid><title>First post</title><link>https://example.com/one</link></item>
</channel></rss>'''


def thread_export():
    return {
        "data": [
            {"id": "102", "author_id": "a", "conversation_id": "100", "created_at": "2026-10-30T13:00:00Z", "text": "Second connected post",
             "referenced_tweets": [{"type": "replied_to", "id": "100"}, {"type": "quoted", "id": "50"}], "attachments": {"media_keys": ["video"]}},
            {"id": "100", "author_id": "a", "conversation_id": "100", "created_at": "2026-10-30T12:00:00Z", "text": "First connected post https://t.co/space",
             "entities": {"urls": [{"url": "https://t.co/space", "expanded_url": "https://x.com/i/spaces/example"}]}},
            {"id": "101", "author_id": "b", "text": "An unrelated reply", "referenced_tweets": [{"type": "replied_to", "id": "100"}]},
            {"id": "999", "author_id": "a", "text": "Disconnected author post"},
        ],
        "includes": {"users": [{"id": "a", "name": "Writer", "username": "writer"}, {"id": "b", "name": "Quoted Author", "username": "other"}],
                     "tweets": [{"id": "50", "author_id": "b", "text": "Context quoted from another author."}],
                     "media": [{"media_key": "video", "type": "video", "preview_image_url": "https://example.com/preview.jpg"}]},
    }


class InboxTest(unittest.TestCase):
    def test_unified_model_and_footnotes(self):
        self.assertIs(Article, ReadingItem)
        item = extract('<article><h1>Footnotes</h1><p>Text<a href="#fn1">1</a></p><p><span id="fn1">A footnote.</span></p></article>', "https://example.com/post?utm_source=rss")
        self.assertEqual(item.source_id, "url:https://example.com/post")
        self.assertIn('href="#note-', item.content)
        anchor = item.content.split('href="#')[1].split('"')[0]
        self.assertIn(f'id="{anchor}"', item.content)

    def test_rss_and_atom(self):
        entries = feed_entries(FEED, "https://example.com/feed")
        self.assertEqual([e["guid"] for e in entries], ["post-two", "post-one"])
        atom = b'<feed xmlns="http://www.w3.org/2005/Atom"><entry><id>urn:post</id><title>Title</title><link href="/post"/><published>2026-10-30</published></entry></feed>'
        self.assertEqual(feed_entries(atom, "https://example.com/feed")[0]["url"], "https://example.com/post")
        with self.assertRaises(ReaderError):
            feed_entries(b'<!DOCTYPE rss [<!ENTITY x "bad">]><rss/>', "https://example.com/feed")

    def test_publication_sync_deduplicates_without_refetching_posts(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary))
            add(cache, "chosen", "https://example.com/feed", "")
            client = Mock(spec=PublicHTTP)
            def fetch(url, *args):
                body = FEED if url.endswith("/feed") else f'<article><h1>{url.rsplit("/", 1)[1]}</h1><p>Readable post content.</p></article>'.encode()
                return Response(body, url, "text/html", "utf-8")
            client.get.side_effect = fetch
            first = sync(cache, client, no_images=True)
            self.assertEqual(len(first["items"]), 2)
            self.assertFalse(first["errors"])
            self.assertEqual(client.get.call_count, 3)
            second = sync(cache, client, no_images=True)
            self.assertFalse(second["items"])
            self.assertEqual(client.get.call_count, 4)
            self.assertEqual(len(cache.entries()), 2)

    def test_local_export_updates_preserve_source_read_progress(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            exported = root / "export"
            exported.mkdir()
            post = exported / "post.html"
            post.write_text('<head><link rel="canonical" href="https://example.com/paid"></head><article><h1>Paid</h1><p>Authorized full post.</p></article>')
            cache = Cache(root / "cache")
            add(cache, "paid", "", str(exported))
            client = Mock(spec=PublicHTTP)
            client.get.side_effect = AssertionError("must remain offline")
            first = sync(cache, client)["items"][0]
            cache.mark(first["id"], "reading", 0.6)
            post.write_text(post.read_text().replace("full post", "updated full post"))
            second = sync(cache, client)["items"][0]
            self.assertEqual(first["id"], second["id"])
            self.assertEqual(second["read_state"], "reading")
            self.assertEqual(second["progress"], 0.6)
            self.assertEqual(second["source_id"], first["source_id"])
            self.assertFalse(sync(cache, client)["items"])

    def test_shared_state_survives_different_cache_variants(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary))
            item = extract('<article><p>Article content.</p></article>', "https://example.com/article")
            data = build_epub(item, "id", PublicHTTP(), no_images=True)
            first = cache.store(cache.key("one", {}), item, data)
            cache.mark(first["id"], "read", 1.0)
            second = cache.store(cache.key("two", {}), item, data)
            self.assertEqual(second["read_state"], "read")
            self.assertEqual(cache.get(second["id"])["progress"], 1.0)

    def test_subscription_dry_run_does_not_write_articles(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary))
            add(cache, "chosen", "https://example.com/feed", "")
            client = Mock(spec=PublicHTTP)
            client.get.side_effect = lambda url, *args: Response(FEED if url.endswith("feed") else b"<p>Actual article.</p>", url, "text/html", None)
            result = sync(cache, client, dry_run=True, limit=1)
            self.assertEqual(len(result["items"]), 1)
            self.assertEqual(cache.entries(), [])

    def test_x_connected_author_posts_quotes_and_previews(self):
        item = thread_item("https://x.com/writer/status/102", thread_export())
        self.assertEqual(item.provenance["post_ids"], "100,102")
        self.assertEqual(item.author, "Writer (@writer)")
        self.assertLess(item.content.index("First connected"), item.content.index("Second connected"))
        self.assertIn("Context quoted", item.content)
        self.assertNotIn("unrelated reply", item.content)
        self.assertNotIn("Disconnected", item.content)
        self.assertIn("Video preview only", item.content)
        self.assertIn("Spaces audio is not embedded", item.content)
        self.assertEqual(item.images, ["https://example.com/preview.jpg"])
        self.assertEqual(json.loads(item.provenance["media_links"]),
                         ["https://x.com/i/spaces/example", "https://x.com/writer/status/102"])
        self.assertEqual(item.provenance["qr_payload"], "https://x.com/writer/status/100")

    def test_x_offline_export_preserves_photo_media_links_once(self):
        export = thread_export()
        first, second = export["data"][1], export["data"][0]
        first["attachments"] = {"media_keys": ["photo-a", "photo-b"]}
        second["attachments"] = {"media_keys": ["photo-a", "video"]}
        export["includes"]["media"].extend([
            {"media_key": "photo-a", "type": "photo", "url": "https://example.com/photo.jpg", "alt_text": "A photo"},
            {"media_key": "photo-b", "type": "photo", "url": "https://example.com/photo.jpg"},
        ])
        item = thread_item("https://x.com/writer/status/102", export)
        self.assertEqual(json.loads(item.provenance["media_links"]),
                         ["https://example.com/photo.jpg", "https://x.com/i/spaces/example",
                          "https://x.com/writer/status/102"])
        self.assertEqual(item.images, ["https://example.com/photo.jpg", "https://example.com/preview.jpg"])

    def test_x_access_errors_are_explicit(self):
        with patch.dict("os.environ", {}, clear=True):
            with self.assertRaisesRegex(ReaderError, "X_BEARER_TOKEN"):
                api_thread("https://x.com/writer/status/100")
        with self.assertRaises(ReaderError):
            post_id("https://example.com/writer/status/100")
        with self.assertRaises(ReaderError):
            thread_item("https://x.com/writer/status/1000", thread_export())
        self.assertEqual(load_export(b'{"meta":{"result_count":0}}')["data"], [])
        cycle = thread_export()
        cycle["data"][1]["referenced_tweets"] = [{"type": "replied_to", "id": "102"}]
        with self.assertRaisesRegex(ReaderError, "Cyclic"):
            thread_item("https://x.com/writer/status/102", cycle)

    def test_official_api_uses_environment_token_and_bounded_requests(self):
        exported = thread_export()
        initial = {"data": exported["data"][1], "includes": exported["includes"]}
        opener = Mock()
        opener.open.side_effect = [io.BytesIO(json.dumps(initial).encode()), io.BytesIO(json.dumps(exported).encode())]
        with patch.dict("os.environ", {"X_BEARER_TOKEN": "authorized-token"}), patch("reader_send.x_adapter.build_opener", return_value=opener):
            payload = api_thread("https://x.com/writer/status/100")
        self.assertEqual(opener.open.call_count, 2)
        requests = [call.args[0] for call in opener.open.call_args_list]
        self.assertTrue(requests[0].full_url.startswith("https://api.x.com/2/tweets/100?"))
        self.assertIn("/tweets/search/recent?", requests[1].full_url)
        self.assertEqual(requests[0].get_header("Authorization"), "Bearer authorized-token")
        self.assertNotIn("authorized-token", json.dumps(payload))
        self.assertEqual(thread_item("https://x.com/writer/status/100", payload).provenance["post_ids"], "100,102")

    def test_x_source_identity_survives_partial_export_refetch(self):
        full = thread_export()
        partial = thread_export()
        partial["data"] = [partial["data"][0]]
        self.assertEqual(thread_item("https://x.com/writer/status/102", full).source_id,
                         thread_item("https://x.com/writer/status/102", partial).source_id)

    def test_x_cli_export_flow_is_offline_and_preserves_state(self):
        with tempfile.TemporaryDirectory() as temporary, patch("sys.stdout", new_callable=io.StringIO), patch("sys.stderr", new_callable=io.StringIO):
            root = Path(temporary)
            export = root / "thread.json"
            export_payload = thread_export()
            export_payload["data"][0]["attachments"] = {"media_keys": ["video", "photo"]}
            export_payload["includes"]["media"].append(
                {"media_key": "photo", "type": "photo", "url": "https://example.com/photo.jpg"})
            export.write_text(json.dumps(export_payload))
            cache_dir = root / "cache"
            with patch.object(PublicHTTP, "get", side_effect=AssertionError("network forbidden")):
                args = ["x", "https://x.com/writer/status/102", "--export", str(export), "--cache-dir", str(cache_dir)]
                self.assertEqual(main(args), 0)
                entry = Cache(cache_dir).entries()[0]
                self.assertIn("https://example.com/photo.jpg", json.loads(entry["provenance"]["media_links"]))
                self.assertEqual(main(["inbox", entry["id"], "--state", "read", "--cache-dir", str(cache_dir)]), 0)
                self.assertEqual(main(args), 0)
                self.assertEqual(Cache(cache_dir).entries()[0]["read_state"], "read")


if __name__ == "__main__":
    unittest.main()
