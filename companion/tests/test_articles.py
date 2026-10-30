import base64
import io
import json
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch
from xml.etree import ElementTree

from PIL import Image

from reader_send.cache import Cache
from reader_send.cli import main
from reader_send.common import ReaderError, filename
from reader_send.epub import MAX_PACKED_IMAGE, build_epub, resize_image
from reader_send.extract import extract
from reader_send.network import PublicHTTP, decode_html
from reader_send.sources import load_sources


class ArticlesTest(unittest.TestCase):
    def image(self, size=(1600, 900)):
        output = io.BytesIO()
        Image.new("RGBA", size, (80, 100, 180, 128)).save(output, "PNG")
        return output.getvalue()

    def test_substack_metadata_and_cleanup(self):
        article = extract('''<html lang="zh-Hans"><head><title>Site title</title>
        <meta property="og:title" content="An essay &amp; a thought"><meta name="author" content="A. Writer">
        <meta property="article:published_time" content="2026-10-30T12:00:00Z">
        <link rel="canonical" href="/p/thought"></head><body><nav>Navigation</nav>
        <article><h1>An essay &amp; a thought</h1><div class="available-content"><p>Hello <em>world</em>.</p>
        <p><a href="/other">Related idea</a> <a href="javascript:alert(1)">safe text</a></p>
        <script>alert('injected')</script><img data-src="/photo.png" alt="Photo"></div>
        <div class="subscribe-widget">Pay us</div></article><footer>Footer</footer></body></html>''', "https://example.com/p/current")
        self.assertEqual(article.title, "An essay & a thought")
        self.assertEqual(article.author, "A. Writer")
        self.assertEqual(article.source_url, "https://example.com/p/thought")
        self.assertEqual(article.language, "zh-Hans")
        self.assertEqual(article.date, "2026-10-30T12:00:00Z")
        self.assertIn("Hello <em>world</em>", article.content)
        self.assertIn('href="https://example.com/other"', article.content)
        for noise in ("Navigation", "Pay us", "Footer", "alert", "javascript"):
            self.assertNotIn(noise, article.content)
        self.assertEqual(article.images, ["/photo.png"])

    def test_json_ld_and_generic_article_selection(self):
        article = extract('''<html><head><script type="application/ld+json">{"@graph":[{"@type":"BlogPosting",
        "headline":"Plain article","author":[{"name":"First"},{"name":"Second"}],"datePublished":"2026-09-12"}]}</script></head>
        <body><nav>Lots of links</nav><main><article><p>A useful paragraph about reading articles.</p>
        <p>Another paragraph with real prose, retained in the EPUB.</p></article></main></body></html>''')
        self.assertEqual(article.title, "Plain article")
        self.assertEqual(article.author, "First, Second")
        self.assertEqual(article.date, "2026-09-12")
        self.assertNotIn("Lots of links", article.content)

    def test_nested_and_hidden_content(self):
        article = extract('<article><h1>Title</h1><p>First<p>Second<div hidden>Secret</div><div style="display: none">Hidden</div><p>Third</article>')
        self.assertNotIn("Secret", article.content)
        self.assertNotIn("Hidden", article.content)
        self.assertNotIn("<h1>", article.content)
        ElementTree.fromstring("<body>" + article.content + "</body>")

    def test_no_readable_content(self):
        with self.assertRaises(ReaderError):
            extract('<html><body><script>app()</script><nav>Menu</nav></body></html>')

    def test_paywall_is_not_bypassed(self):
        article = extract('<article><p>This is a public preview.</p><div class="paywall">Pay now</div></article>')
        self.assertIn("public preview", article.content)
        self.assertNotIn("Pay now", article.content)
        self.assertTrue(article.warnings)

    def test_limits_and_invalid_xml_characters(self):
        with self.assertRaises(ReaderError):
            extract("<div>" * 130 + "content")
        article = extract('<article><p>hello\x00world\ud800</p></article>')
        self.assertNotIn("\x00", article.content)
        self.assertNotIn("\ud800", article.content)

    def test_charset_and_filename(self):
        self.assertEqual(decode_html(b'<meta charset="windows-1252"><p>caf\xe9</p>'), '<meta charset="windows-1252"><p>café</p>')
        name = filename("../../CON: A / title? 中国", "same")
        self.assertRegex(name, r"^[a-z0-9-]+\.epub$")
        self.assertEqual(name, filename("../../CON: A / title? 中国", "same"))
        self.assertLessEqual(len(filename("x" * 1000, "same")), 90)

    def test_images_bounded_and_epub_deterministic(self):
        image = self.image()
        packed = resize_image(image)
        self.assertLessEqual(len(packed), MAX_PACKED_IMAGE)
        with Image.open(io.BytesIO(packed)) as picture:
            self.assertLessEqual(max(picture.size), 1200)
            self.assertEqual(picture.mode, "RGB")
        encoded = base64.b64encode(image).decode()
        article = extract(f'<article><h1>Read &amp; reflect</h1><p>Offline prose.</p><img src="data:image/png;base64,{encoded}" alt="Art"></article>', "https://example.com/article")
        with patch.object(PublicHTTP, "get", side_effect=AssertionError("network forbidden")):
            epub = build_epub(article, "identity", PublicHTTP(), remote_images=False)
            self.assertEqual(epub, build_epub(article, "identity", PublicHTTP(), remote_images=False))
        with zipfile.ZipFile(io.BytesIO(epub)) as archive:
            self.assertEqual(archive.namelist()[0], "mimetype")
            self.assertEqual(archive.getinfo("mimetype").compress_type, zipfile.ZIP_STORED)
            self.assertEqual(archive.read("mimetype"), b"application/epub+zip")
            self.assertIn("OEBPS/images/0.jpg", archive.namelist())
            for name in ("META-INF/container.xml", "OEBPS/content.opf", "OEBPS/toc.ncx", "OEBPS/article.xhtml"):
                ElementTree.fromstring(archive.read(name))
            self.assertNotIn(b"data:image", archive.read("OEBPS/article.xhtml"))

    def test_missing_images_keep_alt_text(self):
        article = extract('<article><p>Prose survives.</p><img src="https://example.com/a.png" alt="Useful description"></article>')
        with patch.object(PublicHTTP, "get", side_effect=AssertionError("network forbidden")):
            epub = build_epub(article, "id", PublicHTTP(), remote_images=False)
        self.assertTrue(article.warnings)
        with zipfile.ZipFile(io.BytesIO(epub)) as archive:
            content = archive.read("OEBPS/article.xhtml")
            self.assertIn(b"Useful description", content)
            self.assertNotIn(b"<img", content)

    def test_local_paths_and_zip_export_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            html_path = root / "post.html"
            html_path.write_text("<p>Offline article.</p>")
            local = next(load_sources(html_path))
            with self.assertRaises(ReaderError):
                local.image("../outside.png")
            with self.assertRaises(ReaderError):
                local.image("file:///etc/passwd")
            archive_path = root / "export.zip"
            with zipfile.ZipFile(archive_path, "w") as archive:
                archive.writestr("posts.csv", "post_id,title,post_date,post_url\n123,Export title,2026-10-30,https://example.com/p/post\n")
                archive.writestr("posts/123.post.html", "<p>Exported full article.</p><img src='../images/art.png'>")
                archive.writestr("images/art.png", self.image((10, 10)))
            source = next(load_sources(archive_path))
            self.assertEqual(source.metadata["title"], "Export title")
            self.assertEqual(source.metadata["date"], "2026-10-30")
            self.assertTrue(source.image("../images/art.png").startswith(b"\x89PNG"))
            with self.assertRaises(ReaderError):
                source.image("../../outside.png")
            with zipfile.ZipFile(root / "unsafe.zip", "w") as archive:
                archive.writestr("../post.html", "<p>Unsafe</p>")
            with self.assertRaises(ReaderError):
                list(load_sources(root / "unsafe.zip"))

    def test_cache_integrity_and_cli_offline_flow(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "post.html"
            source.write_text('<article><h1>Offline title</h1><p>A complete local reading flow.</p></article>')
            cache_dir = root / "cache"
            out = root / "out.epub"
            with patch.object(PublicHTTP, "get", side_effect=AssertionError("network forbidden")), patch("sys.stdout", new_callable=io.StringIO) as stdout:
                self.assertEqual(main(["convert", str(source), "--cache-dir", str(cache_dir), "--output", str(out), "--dry-run"]), 0)
                self.assertFalse(out.exists())
                self.assertFalse(cache_dir.exists())
                self.assertEqual(main(["convert", str(source), "--cache-dir", str(cache_dir), "--output", str(out)]), 0)
                self.assertTrue(out.exists())
                entries = Cache(cache_dir).entries()
                self.assertEqual(len(entries), 1)
                self.assertEqual(main(["inspect", entries[0]["id"], "--cache-dir", str(cache_dir)]), 0)
                self.assertEqual(main(["send", entries[0]["id"], "--cache-dir", str(cache_dir), "--reader", "192.0.2.1:53317", "--dry-run"]), 0)
                self.assertIn('"dry_run": true', stdout.getvalue())
            Path(entries[0]["path"]).write_bytes(b"corrupt")
            with self.assertRaises(ReaderError):
                Cache(cache_dir).get(entries[0]["id"])
            with self.assertRaises(ReaderError):
                Cache(cache_dir).get("../escape")

    def test_public_fetch_cache_avoids_second_http_request(self):
        from reader_send.network import Response
        with tempfile.TemporaryDirectory() as temporary, patch("sys.stdout", new_callable=io.StringIO):
            response = Response(b"<article><h1>Public</h1><p>Public article text.</p></article>", "https://example.com/post", "text/html", "utf-8")
            with patch.object(PublicHTTP, "get", return_value=response) as get:
                args = ["fetch", "https://example.com/post", "--cache-dir", temporary]
                self.assertEqual(main(args), 0)
                self.assertEqual(main(args), 0)
                self.assertEqual(get.call_count, 1)
                self.assertEqual(main(args + ["--refresh"]), 0)
                self.assertEqual(get.call_count, 2)


if __name__ == "__main__":
    unittest.main()
