import json
import tempfile
import time
import unittest
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest.mock import Mock, patch

from reader_send.browser_source import (ARCHIVE_LINKS, BROWSER_PAGE_CHARS, AccountSubscription,
                                        BrowserPage, account_expander, account_rows,
                                        discover_substack_subscriptions, page_from_value, page_warnings,
                                        parse_archive)
from reader_send.cache import Cache
from reader_send.common import ReaderError
from reader_send.network import PublicHTTP, Response
from reader_send.subscriptions import (MANAGED_ORIGIN, REFRESH_STATE, add, publication_time,
                                       refresh_managed_subscriptions, subscription_path,
                                       subscriptions, sync)
from reader_send.x_browser import _Timeout

ARCHIVE = "https://example.substack.com/archive"


def post_html(slug, title, extra=""):
    return (f'<head><link rel="canonical" href="https://example.substack.com/p/{slug}">'
            f"<title>{title}</title></head>"
            f"<article><h1>{title}</h1><p>Readable article body for {slug} with enough text to matter.</p>{extra}"
            "</article>")


def page(slug, title, text="", extra=""):
    return BrowserPage(href=f"https://example.substack.com/p/{slug}",
                       html=post_html(slug, title, extra), text=text)


class ArchiveParseTest(unittest.TestCase):
    def test_keeps_only_visible_same_origin_post_links_in_order(self):
        value = {"links": [
            {"url": "https://example.substack.com/p/newest?utm_source=archive", "text": "Newest"},
            {"url": "https://example.substack.com/p/newest", "text": "Duplicate"},
            {"url": "https://example.substack.com/about", "text": "About"},
            {"url": "https://elsewhere.example/p/foreign", "text": "Foreign"},
            {"url": "https://example.substack.com/p/middle/", "text": "Middle"},
            {"url": "javascript:alert(1)", "text": "Not a URL"},
            {"url": "https://example.substack.com/p/oldest", "text": "Oldest"},
        ]}
        links = parse_archive(value, ARCHIVE)
        self.assertEqual([link["url"] for link in links], [
            "https://example.substack.com/p/newest",
            "https://example.substack.com/p/middle",
            "https://example.substack.com/p/oldest"])
        self.assertEqual([link["text"] for link in links], ["Newest", "Middle", "Oldest"])

    def test_empty_archive_is_an_explicit_failure(self):
        with self.assertRaisesRegex(ReaderError, "no same-origin post links"):
            parse_archive({"links": [{"url": "https://example.substack.com/about"}]}, ARCHIVE)
        with self.assertRaises(ReaderError):
            parse_archive("not a snapshot", ARCHIVE)

    def test_archive_redirected_to_sign_in_or_welcome_names_the_missing_login(self):
        for path in ("/welcome", "/sign-in"):
            with self.assertRaisesRegex(ReaderError, f"redirected from {ARCHIVE} to {path}.*not signed in"):
                parse_archive({"href": f"https://example.substack.com{path}", "links": []}, ARCHIVE)

    def test_archive_scan_is_bounded(self):
        value = {"links": [{"url": f"https://example.substack.com/p/post-{n}"} for n in range(ARCHIVE_LINKS + 25)]}
        self.assertEqual(len(parse_archive(value, ARCHIVE)), ARCHIVE_LINKS)


class PageSnapshotTest(unittest.TestCase):
    def test_rendered_page_keeps_canonical_url_and_visible_text(self):
        result = page_from_value({"href": "https://example.substack.com/p/one?utm=x",
                                  "html": post_html("one", "One"), "text": "One body"}, ARCHIVE)
        self.assertEqual(result.href, "https://example.substack.com/p/one?utm=x")

    def test_oversized_or_empty_snapshot_is_refused_not_truncated(self):
        with self.assertRaisesRegex(ReaderError, "browser-capture bound"):
            page_from_value({"href": ARCHIVE, "html": None, "length": BROWSER_PAGE_CHARS + 1}, ARCHIVE)
        with self.assertRaises(ReaderError):
            page_from_value({"href": ARCHIVE, "html": "", "length": 0}, ARCHIVE)

    def test_invite_only_notice_is_an_actionable_access_error(self):
        with self.assertRaisesRegex(ReaderError, "invite-only notice"):
            page_from_value({"href": ARCHIVE, "html": "<html><body>x</body></html>",
                             "text": "This publication is invite-only"}, ARCHIVE)

    def test_paywall_marker_only_warns_about_the_visible_portion(self):
        self.assertEqual(page_warnings("This post is for paid subscribers only"), [
            "This post is partly paywalled; the EPUB contains only the portion this account can read."])
        self.assertEqual(page_warnings("This post is only available to paying subscribers"), [
            "This post is partly paywalled; the EPUB contains only the portion this account can read."])
        self.assertEqual(page_warnings("Ordinary free post body"), [])

    def test_generic_header_upsell_is_not_mistaken_for_a_paywall(self):
        # Observed on a fully readable free post: the site header offers these to any free reader.
        self.assertEqual(page_warnings("Dawn Rays 2 Upgrade to paid Subscribe to continue reading"), [])


class BrowserSourceSyncTest(unittest.TestCase):
    def configure(self, cache):
        return add(cache, "browseronly", "", "", browser_url=ARCHIVE)

    def test_collects_latest_visible_posts_then_deduplicates_without_refetch(self):
        links = [{"url": "https://example.substack.com/p/newest", "text": "Newest"},
                 {"url": "https://example.substack.com/p/middle", "text": "Middle"},
                 {"url": "https://example.substack.com/p/oldest", "text": "Oldest"}]
        pages = {link["url"]: page(link["url"].rsplit("/", 1)[1], link["text"]) for link in links}
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary))
            self.assertEqual(self.configure(cache)["browser_url"], ARCHIVE)
            client = Mock(spec=PublicHTTP)
            client.get.side_effect = AssertionError("browser source must not use HTTP")
            with patch("reader_send.subscriptions.archive_posts", return_value=links) as archive, \
                    patch("reader_send.subscriptions.post_page", side_effect=lambda url, **kw: pages[url]) as capture:
                first = sync(cache, client, limit=2, no_images=True)
                self.assertEqual([item["source_id"] for item in first["items"]], [
                    "url:https://example.substack.com/p/middle", "url:https://example.substack.com/p/newest"])
                self.assertFalse(first["errors"])
                self.assertEqual(load_provenance(cache)[0]["browser_url"], ARCHIVE)
                self.assertEqual({item["source_type"] for item in first["items"]}, {"substack-browser"})
                self.assertEqual(capture.call_count, 2)

                second = sync(cache, client, limit=2, no_images=True)
                self.assertEqual([item["source_id"] for item in second["items"]],
                                 ["url:https://example.substack.com/p/oldest"])
                self.assertFalse(second["errors"])
                self.assertEqual(archive.call_count, 2)
                self.assertEqual(capture.call_count, 3)

                third = sync(cache, client, limit=2, no_images=True)
                self.assertEqual(third["items"], [])
                self.assertFalse(third["errors"])
                self.assertEqual(capture.call_count, 3)
                self.assertEqual(len(cache.entries()), 3)

    def test_archive_access_failure_is_reported_and_never_swallowed(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary))
            self.configure(cache)
            client = Mock(spec=PublicHTTP)
            with patch("reader_send.subscriptions.archive_posts",
                       side_effect=ReaderError("Chrome did not finish rendering")), \
                    patch("reader_send.subscriptions.post_page") as capture:
                result = sync(cache, client, no_images=True)
            self.assertEqual(result["items"], [])
            self.assertEqual([(error["stage"], error["publication"]) for error in result["errors"]],
                             [("browser", "browseronly")])
            self.assertIn("Chrome did not finish rendering", result["errors"][0]["error"])
            self.assertEqual(capture.call_count, 0)
            self.assertEqual(cache.entries(), [])

    def test_one_blocked_post_keeps_going_and_names_the_url(self):
        links = [{"url": "https://example.substack.com/p/newest", "text": "Newest"},
                 {"url": "https://example.substack.com/p/middle", "text": "Middle"}]
        healthy = page("middle", "Middle")
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary))
            self.configure(cache)
            client = Mock(spec=PublicHTTP)
            def capture(url, **kwargs):
                if url.endswith("/newest"):
                    raise ReaderError(f"Chrome shows an invite-only notice for {url}")
                return healthy
            with patch("reader_send.subscriptions.archive_posts", return_value=links), \
                    patch("reader_send.subscriptions.post_page", side_effect=capture):
                result = sync(cache, client, limit=2, no_images=True)
            self.assertEqual([item["source_id"] for item in result["items"]],
                             ["url:https://example.substack.com/p/middle"])
            self.assertEqual(len(result["errors"]), 1)
            self.assertEqual(result["errors"][0]["stage"], "browser-post")
            self.assertEqual(result["errors"][0]["url"], "https://example.substack.com/p/newest")
            self.assertEqual(len(cache.entries()), 1)

    def test_paywall_warning_reaches_the_cached_item(self):
        links = [{"url": "https://example.substack.com/p/paid", "text": "Paid"}]
        paid = page("paid", "Paid", text="This post is for paid subscribers only")
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary))
            self.configure(cache)
            client = Mock(spec=PublicHTTP)
            with patch("reader_send.subscriptions.archive_posts", return_value=links), \
                    patch("reader_send.subscriptions.post_page", return_value=paid):
                result = sync(cache, client, no_images=True)
            warnings = result["items"][0]["warnings"]
            self.assertTrue(any("partly paywalled" in warning for warning in warnings), warnings)

    def test_dry_run_does_not_write_articles(self):
        links = [{"url": "https://example.substack.com/p/one", "text": "One"}]
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary))
            self.configure(cache)
            client = Mock(spec=PublicHTTP)
            with patch("reader_send.subscriptions.archive_posts", return_value=links), \
                    patch("reader_send.subscriptions.post_page", return_value=page("one", "One")):
                result = sync(cache, client, dry_run=True, no_images=True)
            self.assertEqual(len(result["items"]), 1)
            self.assertTrue(result["items"][0]["dry_run"])
            self.assertEqual(cache.entries(), [])


class BrowserRecordTest(unittest.TestCase):
    def test_browser_only_record_is_valid_and_survives_read_back(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary))
            add(cache, "browseronly", "", "", browser_url=ARCHIVE)
            record = subscriptions(cache)[0]
            self.assertEqual((record["feed"], record["export_dir"], record["browser_url"]), ("", "", ARCHIVE))

    def test_configuration_needs_at_least_one_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary))
            with self.assertRaises(ReaderError):
                add(cache, "empty", "", "")
            with self.assertRaises(ReaderError):
                add(cache, "unsafe", "", "", browser_url="https://user:secret@example.com/archive")


class RecentWindowTest(unittest.TestCase):
    """Recent mode: publication timestamps decide, never 'now', and skips never backfill."""

    def ago(self, days):
        return (datetime.now(timezone.utc) - timedelta(days=days)).isoformat()

    def rss(self, rows):
        items = "".join(f"<item><guid>{guid}</guid><title>{title}</title><link>{url}</link>"
                        f"<pubDate>{date}</pubDate></item>" for guid, title, url, date in rows)
        return f'<rss version="2.0"><channel><title>P</title>{items}</channel></rss>'.encode()

    def test_publication_time_accepts_feed_and_page_formats(self):
        self.assertEqual(publication_time("2026-10-11T23:25:37Z"),
                         datetime(2026, 10, 11, 23, 25, 37, tzinfo=timezone.utc))
        self.assertEqual(publication_time("Sun, 11 Oct 2026 23:25:37 GMT"),
                         datetime(2026, 10, 11, 23, 25, 37, tzinfo=timezone.utc))
        self.assertEqual(publication_time("2026-10-11 23:25:37"),
                         datetime(2026, 10, 11, 23, 25, 37, tzinfo=timezone.utc))
        self.assertEqual(publication_time("2026-10-11T19:25:37-04:00"),
                         datetime(2026, 10, 11, 23, 25, 37, tzinfo=timezone.utc))
        self.assertIsNone(publication_time(""))
        self.assertIsNone(publication_time("not a date"))

    def test_rss_recent_mode_collects_only_fresh_entries_and_flags_undated(self):
        feed = self.rss([("fresh", "Fresh", "https://example.com/fresh", self.ago(1)),
                         ("stale", "Stale", "https://example.com/stale", self.ago(40)),
                         ("undated", "Undated", "https://example.com/undated", "")])
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary))
            add(cache, "publication", "https://example.com/feed", "")
            client = Mock(spec=PublicHTTP)
            def fetch(url, *args):
                body = feed if url.endswith("/feed") else b"<article><h1>Article</h1><p>Readable body text.</p></article>"
                return Response(body, url, "text/html", "utf-8")
            client.get.side_effect = fetch
            result = sync(cache, client, since_days=7, no_images=True)
            self.assertEqual([item["source_url"] for item in result["items"]], ["https://example.com/fresh"])
            self.assertFalse(result["errors"])
            self.assertEqual([(warning["url"], "undated feed entry" in warning["warning"])
                              for warning in result["warnings"]],
                             [("https://example.com/undated", True)])
            fetched = [call.args[0] for call in client.get.call_args_list]
            self.assertNotIn("https://example.com/stale", fetched)
            self.assertNotIn("https://example.com/undated", fetched)
            self.assertTrue((cache.root / "subscription-state" / "publication.json").is_file())

            again = sync(cache, client, since_days=7, no_images=True)
            self.assertEqual(again["items"], [])
            self.assertEqual(again["warnings"], [])
            self.assertEqual([call.args[0] for call in client.get.call_args_list].count("https://example.com/fresh"), 1)

    def test_without_since_days_recent_filtering_is_off(self):
        feed = self.rss([("stale", "Stale", "https://example.com/stale", self.ago(40))])
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary))
            add(cache, "publication", "https://example.com/feed", "")
            client = Mock(spec=PublicHTTP)
            client.get.side_effect = lambda url, *args: Response(
                feed if url.endswith("/feed") else b"<article><h1>Old</h1><p>Readable body text.</p></article>",
                url, "text/html", "utf-8")
            result = sync(cache, client, no_images=True)
            self.assertEqual([item["source_url"] for item in result["items"]], ["https://example.com/stale"])
            self.assertFalse((cache.root / "subscription-state").exists())

    def test_browser_recent_mode_skips_stale_archive_link_without_navigation(self):
        links = [{"url": "https://example.substack.com/p/stale", "text": "Stale", "date": self.ago(30)},
                 {"url": "https://example.substack.com/p/fresh", "text": "Fresh", "date": self.ago(2)}]
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary))
            add(cache, "publication", "", "", browser_url=ARCHIVE)
            client = Mock(spec=PublicHTTP)
            with patch("reader_send.subscriptions.archive_posts", return_value=links), \
                    patch("reader_send.subscriptions.post_page",
                          side_effect=lambda url, **kw: page("fresh", "Fresh")) as capture:
                result = sync(cache, client, since_days=7, no_images=True)
            self.assertEqual([item["source_url"] for item in result["items"]],
                             ["https://example.substack.com/p/fresh"])
            self.assertEqual([call.args[0] for call in capture.call_args_list],
                             ["https://example.substack.com/p/fresh"])
            self.assertEqual(result["warnings"], [])

    def test_browser_recent_mode_undated_post_is_warned_once_and_never_refetched(self):
        links = [{"url": "https://example.substack.com/p/undated", "text": "Undated", "date": ""}]
        undated_page = BrowserPage(
            href="https://example.substack.com/p/undated",
            html='<head><link rel="canonical" href="https://example.substack.com/p/undated"></head>'
                 "<article><h1>Undated</h1><p>Readable body text with no publication date anywhere.</p></article>",
            text="")
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary))
            add(cache, "publication", "", "", browser_url=ARCHIVE)
            client = Mock(spec=PublicHTTP)
            with patch("reader_send.subscriptions.archive_posts", return_value=links), \
                    patch("reader_send.subscriptions.post_page", return_value=undated_page) as capture:
                first = sync(cache, client, since_days=7, no_images=True)
                self.assertEqual(first["items"], [])
                self.assertEqual(len(first["warnings"]), 1)
                self.assertIn("no publication date", first["warnings"][0]["warning"])
                self.assertEqual(capture.call_count, 1)

                second = sync(cache, client, since_days=7, no_images=True)
            self.assertEqual(second["items"], [])
            self.assertEqual(second["warnings"], [])
            self.assertEqual(capture.call_count, 1)
            self.assertEqual(cache.entries(), [])

    def test_browser_recent_mode_collects_undated_link_whose_post_is_fresh(self):
        links = [{"url": "https://example.substack.com/p/fresh", "text": "Fresh", "date": ""}]
        fresh_page = BrowserPage(href="https://example.substack.com/p/fresh",
                                 html='<head><link rel="canonical" href="https://example.substack.com/p/fresh">'
                                      f'<meta property="article:published_time" content="{self.ago(2)}"></head>'
                                      "<article><h1>Fresh</h1><p>Readable body text for the fresh post.</p></article>",
                                 text="")
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary))
            add(cache, "publication", "", "", browser_url=ARCHIVE)
            client = Mock(spec=PublicHTTP)
            with patch("reader_send.subscriptions.archive_posts", return_value=links), \
                    patch("reader_send.subscriptions.post_page", return_value=fresh_page):
                result = sync(cache, client, since_days=7, no_images=True)
            self.assertEqual([item["source_url"] for item in result["items"]],
                             ["https://example.substack.com/p/fresh"])
            self.assertEqual(result["warnings"], [])

    def test_recent_days_must_be_a_bounded_window(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Cache(Path(temporary))
            add(cache, "publication", "https://example.com/feed", "")
            for value in (0, 366):
                with self.assertRaises(ReaderError):
                    sync(cache, Mock(spec=PublicHTTP), since_days=value)


class AccountSettingsParseTest(unittest.TestCase):
    def test_rows_keep_publication_origin_and_strip_membership_label(self):
        rows = account_rows({"rows": [
            {"url": "https://antisociety.substack.com/account", "text": "Antisociety Free subscriber"},
            {"url": "https://newsletter.benji.app/account?lli=1", "text": "Benji Free subscriber"},
            {"url": "https://example.com/about", "text": "About"},
            {"url": "javascript:void(0)", "text": "Muted words"},
        ]})
        self.assertEqual([(row.origin, row.name, row.private) for row in rows], [
            ("https://antisociety.substack.com", "Antisociety", False),
            ("https://newsletter.benji.app", "Benji", False)])
        self.assertEqual(rows[1].url, "https://newsletter.benji.app/account?lli=1")

    def test_duplicate_publication_rows_are_ambiguous(self):
        with self.assertRaisesRegex(ReaderError, "twice"):
            account_rows({"rows": [{"url": "https://a.substack.com/account", "text": "A Free subscriber"},
                                   {"url": "https://a.substack.com/account?lli=1", "text": "A Free subscriber"}]})

    def test_private_flag_is_only_what_the_row_itself_says(self):
        hidden = account_rows({"rows": [{"url": "https://hidden.substack.com/account",
                                         "text": "Hidden Invite-only subscriber"}]})
        self.assertTrue(hidden[0].private)
        open_row = account_rows({"rows": [{"url": "https://open.substack.com/account",
                                           "text": "Open Free subscriber"}]})
        self.assertFalse(open_row[0].private)

    def test_expander_count_is_read_or_absent(self):
        self.assertEqual(account_expander({"more": "Show 34 more"}), 34)
        self.assertIsNone(account_expander({"more": ""}))
        self.assertIsNone(account_expander({"more": "Load more"}))


class FakeSettingsSession:
    def __init__(self, snapshots, click=True):
        self.snapshots = list(snapshots)
        self.click = click
        self.clicks = 0

    def call(self, method, params, deadline):
        if "button.click()" in params.get("expression", ""):
            self.clicks += 1
            return {"result": {"value": self.click}}
        value = self.snapshots.pop(0) if len(self.snapshots) > 1 else self.snapshots[0]
        return {"result": {"value": value}}


@contextmanager
def fake_settings_tab(url, *, endpoint, timeout):
    yield fake_settings_tab.session, time.monotonic() + timeout, url, timeout


def listed_row(host, name):
    return {"url": f"https://{host}/account", "text": f"{name} Free subscriber"}


def settings_state(rows, more=""):
    return {"href": "https://substack.com/settings", "title": "Settings", "more": more,
            "body": "Subscriptions", "rows": rows}


class AccountDiscoveryTest(unittest.TestCase):
    def discover(self, snapshots, click=True, timeout=1.0):
        fake_settings_tab.session = FakeSettingsSession(snapshots, click=click)
        with patch("reader_send.browser_source._tab", fake_settings_tab):
            return discover_substack_subscriptions(timeout=timeout)

    def test_complete_list_needs_expansion_and_no_leftover_disclosure(self):
        first = [listed_row("one.substack.com", "One"), listed_row("two.substack.com", "Two")]
        everything = first + [listed_row("three.substack.com", "Three")]
        rows = self.discover([settings_state(first, "Show 1 more"), settings_state(everything)])
        self.assertEqual([row.origin for row in rows], [
            "https://one.substack.com", "https://two.substack.com", "https://three.substack.com"])
        self.assertEqual(fake_settings_tab.session.clicks, 1)

    def test_rows_without_any_disclosure_are_already_complete(self):
        rows = self.discover([settings_state([listed_row("one.substack.com", "One")])])
        self.assertEqual([row.name for row in rows], ["One"])
        self.assertEqual(fake_settings_tab.session.clicks, 0)

    def test_count_mismatch_fails_closed(self):
        first = [listed_row("one.substack.com", "One")]
        extra = first + [listed_row("two.substack.com", "Two"), listed_row("three.substack.com", "Three")]
        with self.assertRaisesRegex(ReaderError, "incomplete"):
            self.discover([settings_state(first, "Show 1 more"), settings_state(extra)])

    def test_list_that_does_not_settle_fails_closed(self):
        first = [listed_row("one.substack.com", "One")]
        with self.assertRaisesRegex(ReaderError, "did not finish expanding"):
            self.discover([settings_state(first, "Show 2 more"),
                           settings_state(first + [listed_row("two.substack.com", "Two")], "Show 1 more")],
                          timeout=0.4)

    def test_missing_expander_is_an_error_not_a_partial_list(self):
        with self.assertRaisesRegex(ReaderError, "could not activate"):
            self.discover([settings_state([listed_row("one.substack.com", "One")], "Show 2 more")], click=False)

    def test_signed_out_page_is_actionable(self):
        with self.assertRaisesRegex(ReaderError, "Sign in to Substack"):
            self.discover([{"href": "https://substack.com/login", "title": "Sign in", "more": "",
                            "body": "Sign in to Substack", "rows": []}], timeout=0.3)

    def test_sign_in_redirect_stays_actionable_when_the_next_snapshot_times_out(self):
        class TimesOut:
            calls = 0

            def call(self, method, params, deadline):
                self.calls += 1
                if self.calls > 1:
                    raise _Timeout
                return {"result": {"value": {"href": "https://substack.com/sign-in?redirect=%2Fsettings",
                                             "title": "Sign in to Substack", "more": "", "body": "", "rows": []}}}

        fake_settings_tab.session = TimesOut()
        with patch("reader_send.browser_source._tab", fake_settings_tab):
            with self.assertRaisesRegex(ReaderError, "Sign in to Substack once"):
                discover_substack_subscriptions(timeout=5.0)

    def test_settings_without_rows_fails_instead_of_claiming_an_empty_list(self):
        with self.assertRaisesRegex(ReaderError, "no subscription rows"):
            self.discover([settings_state([])], timeout=0.3)


NOW = datetime(2026, 10, 30, 12, tzinfo=timezone.utc)


def account(host, name, private=False):
    return AccountSubscription(url=f"https://{host}/account", origin=f"https://{host}",
                               name=name, private=private)


class ManagedRefreshTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cache = Cache(self.root)
        self.export = self.root / "export"
        self.export.mkdir()

    def write(self, name, **fields):
        record = {"name": name, "feed": "", "export_dir": "", "browser_url": ""}
        record.update(fields)
        path = subscription_path(self.cache, name)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(record, indent=2) + "\n")
        return path

    def refresh(self, rows, **kwargs):
        with patch("reader_send.subscriptions.discover_substack_subscriptions",
                   return_value=rows) as discovery:
            return refresh_managed_subscriptions(self.cache, now=kwargs.pop("now", NOW), **kwargs), discovery

    def test_refresh_runs_at_most_once_per_interval_without_the_browser(self):
        self.write("one-substack-com", feed="https://one.substack.com/feed")
        rows = [account("one.substack.com", "One")]
        first, discovery = self.refresh(rows, interval=86400)
        self.assertEqual(first["status"], "refreshed")
        self.assertEqual(discovery.call_count, 1)
        second, discovery = self.refresh(rows, now=NOW + timedelta(hours=1))
        self.assertEqual(second["status"], "not-due")
        discovery.assert_not_called()

    def test_complete_list_marks_choices_preserves_them_and_disables_only_missing_managed(self):
        self.write("one-substack-com", feed="https://one.substack.com/feed")
        self.write("dawn-substack-com", feed="", browser_url="https://dawn.substack.com/archive")
        self.write("manual-only", export_dir=str(self.export))
        self.write("gone-substack-com", feed="https://gone.substack.com/feed",
                   managed_origin=MANAGED_ORIGIN, managed_enabled=True)
        result, _ = self.refresh([account("one.substack.com", "One"),
                                  account("dawn.substack.com", "Dawn"),
                                  account("fresh.example", "Fresh")], interval=0)
        records = {entry["name"]: entry for entry in subscriptions(self.cache, include_disabled=True)}
        self.assertEqual(records["one-substack-com"]["feed"], "https://one.substack.com/feed")
        self.assertEqual(records["one-substack-com"]["managed_origin"], MANAGED_ORIGIN)
        self.assertTrue(records["one-substack-com"]["managed_enabled"])
        self.assertEqual(records["dawn-substack-com"]["browser_url"], "https://dawn.substack.com/archive")
        self.assertEqual(records["dawn-substack-com"]["feed"], "")
        self.assertNotIn("managed_origin", records["manual-only"])
        self.assertFalse(records["gone-substack-com"]["managed_enabled"])
        self.assertEqual(records["fresh-example"]["feed"], "https://fresh.example/feed")
        self.assertEqual(result["added"], ["fresh-example"])
        self.assertEqual(result["disabled"], ["gone-substack-com"])
        self.assertIn("one-substack-com", result["updated"])
        self.assertEqual([entry["name"] for entry in subscriptions(self.cache)],
                         ["dawn-substack-com", "fresh-example", "manual-only", "one-substack-com"])
        self.assertEqual(self.cache.entries(), [])

    def test_failed_discovery_changes_nothing_and_keeps_the_refresh_due(self):
        path = self.write("one-substack-com", feed="https://one.substack.com/feed")
        before = path.read_bytes()
        with patch("reader_send.subscriptions.discover_substack_subscriptions",
                   side_effect=ReaderError("Chrome shows a signed-out Substack page")):
            with self.assertRaisesRegex(ReaderError, "signed-out"):
                refresh_managed_subscriptions(self.cache, now=NOW)
        self.assertEqual(path.read_bytes(), before)
        self.assertFalse((self.root / "subscription-state" / REFRESH_STATE).exists())

    def test_new_name_collision_fails_before_any_write(self):
        path = self.write("fresh-example", feed="https://other.example/feed")
        before = path.read_bytes()
        with self.assertRaisesRegex(ReaderError, "already names"):
            self.refresh([account("fresh.example", "Fresh")], interval=0)
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual([entry["name"] for entry in subscriptions(self.cache)], ["fresh-example"])

    def test_private_row_gets_a_browser_source_instead_of_guessing_rss(self):
        result, _ = self.refresh([account("hidden.example", "Hidden", private=True)], interval=0)
        record = subscriptions(self.cache)[0]
        self.assertEqual(record["feed"], "")
        self.assertEqual(record["browser_url"], "https://hidden.example/archive")
        self.assertEqual(result["added"], ["hidden-example"])

    def test_returning_publication_is_reenabled_without_duplication(self):
        self.write("back-substack-com", feed="https://back.substack.com/feed",
                   managed_origin=MANAGED_ORIGIN, managed_enabled=False)
        result, _ = self.refresh([account("back.substack.com", "Back")], interval=0)
        records = subscriptions(self.cache)
        self.assertEqual([entry["name"] for entry in records], ["back-substack-com"])
        self.assertTrue(records[0]["managed_enabled"])
        self.assertEqual(result["enabled"], ["back-substack-com"])
        self.assertEqual(result["added"], [])

    def test_disabled_managed_publication_is_skipped_by_sync(self):
        self.write("gone-substack-com", feed="https://gone.substack.com/feed",
                   managed_origin=MANAGED_ORIGIN, managed_enabled=False)
        self.assertEqual(subscriptions(self.cache), [])
        self.assertEqual([entry["name"] for entry in subscriptions(self.cache, include_disabled=True)],
                         ["gone-substack-com"])
        with self.assertRaisesRegex(ReaderError, "No matching publications"):
            sync(self.cache, Mock(spec=PublicHTTP), no_images=True)


def load_provenance(cache):
    return [record["provenance"] for record in cache.entries()]

if __name__ == "__main__":
    unittest.main()
