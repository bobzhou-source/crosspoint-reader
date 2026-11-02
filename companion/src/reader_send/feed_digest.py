"""Daily X digests with a machine-readable feed manifest (format ``reader-feed-v1``).

One X post or thread is one *post*. Posts are grouped by their local day (``America/New_York`` by
default) into volumes of at most ``MAX_POSTS`` posts and ``MAX_BYTES`` of member content. Volume 1 of
a day is ``X — Sun 2 Nov 2026``; later volumes of the same day (late posts, overflow) are
``X — 2 Nov (2)``, ``X — 2 Nov (3)``... A built volume is never rewritten.

Each post is its own spine document (``posts/0001.xhtml``) so a reader annotation's ``chapterHref``
identifies exactly one post. ``META-INF/feed.json`` lists every post with its item id, stable key,
source, author, handle, timestamp, URL and section href; the server builder
(``portfolio/server/digest.js``) writes the same contract. The build is a pure function of its
input, so retrying it produces identical bytes.
"""
from __future__ import annotations

import html
import io
import json
import re
import zipfile
from dataclasses import dataclass, field
from datetime import date, datetime, timezone
from pathlib import PurePosixPath
from xml.etree import ElementTree
from zoneinfo import ZoneInfo

from .common import ReaderError
from .epub import CONTAINER_XML, pack_epub

FORMAT = "reader-feed-v1"
DEFAULT_TZ = "America/New_York"
MAX_POSTS = 100
MAX_BYTES = 6 * 1024 * 1024
MAX_SINGLE_BYTES = 8 * 1024 * 1024
STATUS_URL = re.compile(r"https://(?:x|twitter)\.com/([A-Za-z0-9_]{1,15})/status/([0-9]{1,25})")
CREATOR = re.compile(r"(.*) \(@([A-Za-z0-9_]{1,15})\)")
ISO = r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z"
POST_HEADING = re.compile(r"<h2>Post ([0-9]+)</h2>")
POST_META = re.compile(r"<p>(" + ISO + r") · <a href=\"([^\"]*)\">Original post</a></p>")
TCO_ANCHOR = re.compile(r"<a href=\"https?://t\.co/[A-Za-z0-9]+\">[^<]*</a>")
TCO_TEXT = re.compile(r"\s*https?://t\.co/[A-Za-z0-9]+")
IMAGE_SRC = re.compile(r"src=\"([^\"]+)\"")
NS = {"opf": "http://www.idpf.org/2007/opf", "dc": "http://purl.org/dc/elements/1.1/",
      "c": "urn:oasis:names:tc:opendocument:xmlns:container"}
WEEKDAYS = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")
MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
STYLE = ("body{margin:0 4%;line-height:1.4}h1{font-size:1.3em}h2{font-size:1.1em;margin:0.2em 0}"
         "h2.author{margin-top:0}p.meta,p.t,p.n{font-size:0.85em;margin:0.1em 0}"
         "blockquote{margin:0.6em 0 0.6em 0.8em;padding-left:0.6em;border-left:2px solid #000}"
         "figure{margin:0.5em 0}img{max-width:100%}ol{padding-left:1.2em}li{margin:0.2em 0}")


@dataclass(frozen=True)
class Post:
    key: str
    item_id: str | None
    author: str
    handle: str
    published: datetime
    url: str
    title: str
    body: str
    images: tuple[tuple[str, bytes], ...] = field(default=())

    @property
    def size(self) -> int:
        return len(self.body.encode("utf-8")) + sum(len(data) for _, data in self.images)


@dataclass(frozen=True)
class Volume:
    source: str
    day: date
    number: int
    posts: tuple[Post, ...]

    @property
    def title(self) -> str:
        return volume_title(self.day, self.number)

    @property
    def filename(self) -> str:
        return self.title + ".epub"


def volume_title(day: date, number: int) -> str:
    if number == 1:
        return f"X — {WEEKDAYS[day.weekday()]} {day.day} {MONTHS[day.month - 1]} {day.year}"
    return f"X — {day.day} {MONTHS[day.month - 1]} ({number})"


def _iso(moment: datetime) -> str:
    return moment.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _member(archive: zipfile.ZipFile, name: str) -> bytes:
    info = archive.getinfo(name)
    if info.file_size > MAX_SINGLE_BYTES:
        raise ReaderError(f"EPUB member {name} is too large")
    return archive.read(info)


def read_single(data: bytes, item_id: str | None = None) -> Post:
    """Parse one companion-generated X post/thread EPUB; ReaderError if it is not one."""
    if len(data) > MAX_SINGLE_BYTES:
        raise ReaderError("EPUB exceeds 8 MiB")
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            container = ElementTree.fromstring(_member(archive, "META-INF/container.xml"))
            rootfile = container.find(".//c:rootfile", NS)
            opf_path = rootfile.get("full-path") if rootfile is not None else None
            if not opf_path:
                raise ReaderError("EPUB has no package document")
            opf = ElementTree.fromstring(_member(archive, opf_path))
            source = (opf.findtext(".//dc:source", "", NS) or "").strip()
            match = STATUS_URL.fullmatch(source)
            creator = CREATOR.fullmatch((opf.findtext(".//dc:creator", "", NS) or "").strip())
            if match is None or creator is None:
                raise ReaderError("not an X post EPUB")
            manifest = {item.get("id"): item.get("href") for item in opf.findall(".//opf:manifest/opf:item", NS)}
            spine = [ref.get("idref") for ref in opf.findall(".//opf:spine/opf:itemref", NS)]
            if len(spine) != 1 or not manifest.get(spine[0]):
                raise ReaderError("not a single-document X post EPUB")
            base = PurePosixPath(opf_path).parent
            doc_path = (base / manifest[spine[0]]).as_posix()
            text = _member(archive, doc_path).decode("utf-8")
            published = re.search(r"<p>(" + ISO + r")</p>", text)
            start = text.find("<div><h2>Post 1</h2>")
            end = text.rfind("</body>")
            if published is None or start < 0 or end < start:
                raise ReaderError("X post EPUB has an unexpected layout")
            body = text[start:end]
            images = []
            doc_dir = PurePosixPath(doc_path).parent
            for src in dict.fromkeys(IMAGE_SRC.findall(body)):
                images.append((src, _member(archive, (doc_dir / src).as_posix())))
    except (zipfile.BadZipFile, KeyError, ElementTree.ParseError, UnicodeDecodeError) as exc:
        raise ReaderError(f"not a readable X post EPUB: {exc}") from None
    title = html.unescape((opf.findtext(".//dc:title", "", NS) or "").strip())
    moment = datetime.strptime(published.group(1), "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    return Post(key=f"x:{match.group(2)}", item_id=item_id, author=creator.group(1).strip() or match.group(1),
                handle=creator.group(2), published=moment, url=source, title=TCO_TEXT.sub("", title).strip(),
                body=body, images=tuple(images))


def local_day(post: Post, tz: ZoneInfo) -> date:
    return post.published.astimezone(tz).date()


def plan_volumes(posts: list[Post], tz_name: str = DEFAULT_TZ, *, existing: dict[date, int] | None = None,
                 max_posts: int = MAX_POSTS, max_bytes: int = MAX_BYTES) -> list[Volume]:
    """Group posts into volumes by local day; ``existing`` maps a day to volumes already built.

    Duplicate keys (the same post delivered twice) keep only the first occurrence in input order.
    """
    tz = ZoneInfo(tz_name)
    seen: set[str] = set()
    days: dict[date, list[Post]] = {}
    for post in posts:
        if post.key in seen:
            continue
        seen.add(post.key)
        days.setdefault(local_day(post, tz), []).append(post)
    volumes = []
    for day in sorted(days):
        number = (existing or {}).get(day, 0)
        batch: list[Post] = []
        size = 0
        for post in sorted(days[day], key=lambda p: (p.published, p.key)):
            if batch and (len(batch) >= max_posts or size + post.size > max_bytes):
                number += 1
                volumes.append(Volume("x", day, number, tuple(batch)))
                batch, size = [], 0
            batch.append(post)
            size += post.size
        if batch:
            number += 1
            volumes.append(Volume("x", day, number, tuple(batch)))
    return volumes


def _clock(moment: datetime, tz: ZoneInfo, day: date) -> str:
    local = moment.astimezone(tz)
    clock = local.strftime("%H:%M")
    if local.date() != day:
        clock = f"{WEEKDAYS[local.weekday()]} {local.day} {MONTHS[local.month - 1]} {clock}"
    return clock


def _post_document(post: Post, number: int, tz: ZoneInfo, day: date) -> tuple[str, dict[str, str]]:
    """Return the post XHTML and a map of its original image src -> digest image path."""
    images = {src: f"images/{number:04d}-{index}{PurePosixPath(src).suffix or '.jpg'}"
              for index, (src, _) in enumerate(post.images)}
    total = len(POST_HEADING.findall(post.body))
    first = [True]

    def meta(match: re.Match) -> str:
        if first[0]:
            first[0] = False
            return ""
        moment = datetime.strptime(match.group(1), "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
        return f'<p class="t"><a href="{match.group(2)}">{_clock(moment, tz, day)}</a></p>'

    body = POST_HEADING.sub(lambda m: "" if total <= 1 else f'<p class="n">{m.group(1)}/{total}</p>', post.body)
    body = POST_META.sub(meta, body)
    body = TCO_ANCHOR.sub("", body)
    body = re.sub(r">([^<]*)<", lambda m: ">" + TCO_TEXT.sub("", m.group(1)) + "<", body)
    body = IMAGE_SRC.sub(lambda m: f'src="../{images[m.group(1)]}"' if m.group(1) in images else m.group(0), body)
    author = html.escape(post.author, quote=False)
    url = html.escape(post.url)
    document = ('<?xml version="1.0" encoding="UTF-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en">'
                f'<head><title>{author} (@{post.handle})</title>'
                '<link rel="stylesheet" type="text/css" href="../style.css"/></head><body><section id="post">'
                f'<h2 class="author">{author}</h2><p class="meta">@{post.handle} · '
                f'<a href="{url}">{_clock(post.published, tz, day)}</a></p>{body}</section></body></html>')
    return document, images


def _summary(post: Post, limit: int = 90) -> str:
    text = re.sub(r"\s+", " ", post.title).strip() or "(post)"
    return text if len(text) <= limit else text[: limit - 1].rstrip() + "…"


def layout(volume: Volume) -> list[Post]:
    """Contents order: authors alphabetically (case-insensitive, then handle), posts by time."""
    return sorted(volume.posts, key=lambda p: (p.author.casefold(), p.handle.casefold(), p.published, p.key))


def manifest(volume: Volume, tz_name: str, *, digest_id: str | None = None, generator: str = "companion") -> dict:
    posts = layout(volume)
    return {
        "format": FORMAT, "kind": "digest", "id": digest_id, "source": volume.source,
        "day": volume.day.isoformat(), "volume": volume.number, "timezone": tz_name, "title": volume.title,
        "generator": generator,
        "items": [{"id": post.item_id, "key": post.key, "source": "x", "author": post.author,
                   "handle": post.handle, "published": _iso(post.published), "url": post.url,
                   "href": f"posts/{index:04d}.xhtml", "title": _summary(post, 140)}
                  for index, post in enumerate(posts, 1)],
    }


def build_digest(volume: Volume, tz_name: str = DEFAULT_TZ, *, digest_id: str | None = None,
                 generator: str = "companion") -> bytes:
    tz = ZoneInfo(tz_name)
    posts = layout(volume)
    feed = manifest(volume, tz_name, digest_id=digest_id, generator=generator)
    title = html.escape(volume.title, quote=False)
    identifier = f"urn:uuid:{digest_id}" if digest_id else f"urn:reader-feed:x:{volume.day.isoformat()}:{volume.number}"
    entries: dict[str, bytes | str] = {"mimetype": "application/epub+zip", "META-INF/container.xml": CONTAINER_XML,
                                       "META-INF/feed.json": json.dumps(feed, ensure_ascii=False, sort_keys=True,
                                                                        separators=(",", ":")) + "\n"}
    documents: list[tuple[str, str]] = []
    image_entries: list[tuple[str, bytes]] = []
    for index, post in enumerate(posts, 1):
        document, images = _post_document(post, index, tz, volume.day)
        documents.append((f"posts/{index:04d}.xhtml", document))
        image_entries.extend((images[src], data) for src, data in post.images)
    groups: list[tuple[Post, list[int]]] = []
    for index, post in enumerate(posts, 1):
        if groups and (groups[-1][0].author, groups[-1][0].handle) == (post.author, post.handle):
            groups[-1][1].append(index)
        else:
            groups.append((post, [index]))
    authors = len(groups)
    contents = [f'<h1>{title}</h1><p class="meta">{len(posts)} post{"s" if len(posts) != 1 else ""} from '
                f'{authors} account{"s" if authors != 1 else ""}</p>']
    nav, toc, order = [], [], 2
    for head, indexes in groups:
        name = html.escape(head.author, quote=False)
        contents.append(f"<h2>{name} <span>@{head.handle}</span></h2><ol>")
        children = []
        first_order = order
        for index in indexes:
            post = posts[index - 1]
            label = html.escape(f"{_clock(post.published, tz, volume.day)} — {_summary(post)}", quote=False)
            contents.append(f'<li><a href="posts/{index:04d}.xhtml">{label}</a></li>')
            children.append(f'<navPoint id="n{index:04d}" playOrder="{order}"><navLabel><text>{label}</text>'
                            f'</navLabel><content src="posts/{index:04d}.xhtml"/></navPoint>')
            order += 1
        contents.append("</ol>")
        # The author entry and its first post share a target, so they share a play order.
        nav.append(f'<navPoint id="a{indexes[0]:04d}" playOrder="{first_order}"><navLabel><text>{name} '
                   f'(@{head.handle})</text></navLabel><content src="posts/{indexes[0]:04d}.xhtml"/>'
                   + "".join(children) + "</navPoint>")
        toc.append(f'<li><a href="posts/{indexes[0]:04d}.xhtml">{name} (@{head.handle})</a><ol>'
                   + "".join(item for item in contents[-len(indexes) - 1:-1]) + "</ol></li>")
    ncx_points = "".join(nav)
    nav_doc = ('<?xml version="1.0" encoding="UTF-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml" '
               'xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="en"><head><title>Contents</title></head><body>'
               f'<nav epub:type="toc" id="toc"><h1>{title}</h1><ol><li><a href="contents.xhtml">Contents</a></li>'
               f'{"".join(toc)}</ol></nav></body></html>')
    contents_doc = ('<?xml version="1.0" encoding="UTF-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en">'
                    f'<head><title>{title}</title><link rel="stylesheet" type="text/css" href="style.css"/></head>'
                    f'<body>{"".join(contents)}</body></html>')
    items = ['<item id="contents" href="contents.xhtml" media-type="application/xhtml+xml"/>']
    items += [f'<item id="p{i:04d}" href="{href}" media-type="application/xhtml+xml"/>' for i, (href, _) in
              enumerate(documents, 1)]
    items += [f'<item id="i{i}" href="{name}" media-type="{_media_type(name)}"/>' for i, (name, _) in
              enumerate(image_entries, 1)]
    items += ['<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>',
              '<item id="style" href="style.css" media-type="text/css"/>',
              '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>']
    spine = '<itemref idref="contents"/>' + "".join(f'<itemref idref="p{i:04d}"/>' for i in range(1, len(documents) + 1))
    # EPUB 3 (posts carry HTML5 figure/section markup) with an NCX kept for older reading systems.
    opf = ('<?xml version="1.0" encoding="UTF-8"?>\n<package xmlns="http://www.idpf.org/2007/opf" version="3.0" '
           'unique-identifier="book-id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
           f'<dc:identifier id="book-id">{identifier}</dc:identifier><dc:title>{title}</dc:title>'
           f'<dc:language>en</dc:language><dc:creator>X</dc:creator><dc:date>{volume.day.isoformat()}</dc:date>'
           f'<meta property="dcterms:modified">{volume.day.isoformat()}T00:00:00Z</meta>'
           '<meta name="reader-feed" content="META-INF/feed.json"/>'
           f'</metadata><manifest>{"".join(items)}</manifest><spine toc="ncx">{spine}</spine></package>')
    ncx = ('<?xml version="1.0" encoding="UTF-8"?>\n<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">'
           f'<head><meta name="dtb:uid" content="{identifier}"/><meta name="dtb:depth" content="2"/>'
           '<meta name="dtb:totalPageCount" content="0"/><meta name="dtb:maxPageNumber" content="0"/></head>'
           f'<docTitle><text>{title}</text></docTitle><navMap><navPoint id="contents" playOrder="1"><navLabel>'
           f'<text>Contents</text></navLabel><content src="contents.xhtml"/></navPoint>{ncx_points}</navMap></ncx>')
    entries["OEBPS/content.opf"] = opf
    entries["OEBPS/toc.ncx"] = ncx
    entries["OEBPS/style.css"] = STYLE
    entries["OEBPS/contents.xhtml"] = contents_doc
    entries["OEBPS/nav.xhtml"] = nav_doc
    for href, document in documents:
        entries["OEBPS/" + href] = document
    for name, data in image_entries:
        entries["OEBPS/" + name] = data
    return pack_epub(entries)


def _media_type(name: str) -> str:
    suffix = PurePosixPath(name).suffix.lower()
    return {".png": "image/png", ".gif": "image/gif", ".webp": "image/webp"}.get(suffix, "image/jpeg")


def read_manifest(data: bytes) -> dict | None:
    """Return the ``reader-feed-v1`` manifest of a digest EPUB, or None for an ordinary book."""
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            value = json.loads(_member(archive, "META-INF/feed.json"))
    except (zipfile.BadZipFile, KeyError, ValueError, ReaderError):
        return None
    return value if isinstance(value, dict) and value.get("format") == FORMAT else None
