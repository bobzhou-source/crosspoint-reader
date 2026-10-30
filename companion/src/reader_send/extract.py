from __future__ import annotations

import html
import json
import re
from dataclasses import dataclass, field
from html.parser import HTMLParser
from urllib.parse import unquote, urljoin, urlsplit

from .common import MAX_IMAGES, ReaderError, clean_text, digest, http_url, xml_text
from .model import ReadingItem

VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"}
DROP = {"head", "script", "style", "noscript", "nav", "aside", "footer", "form", "button", "input", "select", "textarea", "iframe", "object", "embed", "svg", "canvas", "audio", "video", "template"}
NOISE = re.compile(r"(?:^|[\s_-])(?:subscribe|subscription|paywall|comments?|comment-thread|share|sharing|social|newsletter-signup|cookie|advertisement|ads|navigation|recommendations|related-posts|post-footer)(?:$|[\s_-])", re.I)
MAIN_CLASSES = {"available-content", "post-content", "entry-content", "article-content", "article-body", "post-body", "body-markup"}
ALLOWED = {"p", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote", "ul", "ol", "li", "pre", "code", "em", "strong", "b", "i", "u", "s", "sub", "sup", "br", "hr", "a", "figure", "figcaption", "table", "thead", "tbody", "tfoot", "tr", "td", "th", "dl", "dt", "dd", "div", "span"}


@dataclass(eq=False)
class Node:
    tag: str
    attrs: dict[str, str] = field(default_factory=dict)
    children: list[Node | str] = field(default_factory=list)

    def text(self) -> str:
        return "".join(child if isinstance(child, str) else child.text() for child in self.children)


class Document(HTMLParser):
    def __init__(self, source: str):
        super().__init__(convert_charrefs=True)
        self.root = Node("document")
        self.stack = [self.root]
        self.nodes = [self.root]
        self.feed(source)
        self.close()

    def handle_starttag(self, tag, attrs):
        if len(self.nodes) >= 100000 or len(self.stack) >= 128:
            raise ReaderError("HTML is too complex (100,000 nodes / 128 nesting levels maximum)")
        # HTMLParser is not a browser tree builder; handle common omitted closes.
        if tag in {"p", "div", "section", "h1", "h2", "h3", "ul", "ol", "table"} and self.stack[-1].tag == "p":
            self.stack.pop()
        if tag in {"li", "tr", "td", "th"} and self.stack[-1].tag == tag:
            self.stack.pop()
        node = Node(tag, {key: value or "" for key, value in attrs})
        self.stack[-1].children.append(node)
        self.nodes.append(node)
        if tag not in VOID:
            self.stack.append(node)

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        if tag not in VOID:
            self.handle_endtag(tag)

    def handle_endtag(self, tag):
        for i in range(len(self.stack) - 1, 0, -1):
            if self.stack[i].tag == tag:
                del self.stack[i:]
                break

    def handle_data(self, data):
        self.stack[-1].children.append(data)


# Retained for existing companion API consumers; both names are one model.
Article = ReadingItem


def excluded(node: Node) -> bool:
    return (node.tag in DROP or "hidden" in node.attrs
            or node.attrs.get("aria-hidden", "").lower() == "true"
            or bool(re.search(r"(?:display\s*:\s*none|visibility\s*:\s*hidden)", node.attrs.get("style", ""), re.I))
            or bool(NOISE.search(node.attrs.get("class", "") + " " + node.attrs.get("id", ""))))


def json_articles(value):
    if isinstance(value, list):
        for item in value:
            yield from json_articles(item)
    elif isinstance(value, dict):
        kind = value.get("@type", "")
        kinds = kind if isinstance(kind, list) else [kind]
        if any(k in ("Article", "NewsArticle", "BlogPosting", "ReportageNewsArticle") for k in kinds):
            yield value
        if "@graph" in value:
            yield from json_articles(value["@graph"])


def extract(source: str, source_url: str = "", *, title: str = "", author: str = "", date: str = "") -> Article:
    document = Document(source)
    meta: dict[str, str] = {}
    structured: dict = {}
    for node in document.nodes:
        if node.tag == "meta":
            key = (node.attrs.get("property") or node.attrs.get("name") or "").lower()
            meta.setdefault(key, node.attrs.get("content", ""))
        if node.tag == "script" and node.attrs.get("type", "").lower() == "application/ld+json":
            try:
                for entry in json_articles(json.loads(node.text())):
                    if not structured:
                        structured = entry
            except (ValueError, RecursionError):
                pass
    def first_text(tag):
        return next((clean_text(n.text()) for n in document.nodes if n.tag == tag and clean_text(n.text())), "")
    title = clean_text(title or meta.get("og:title") or str(structured.get("headline") or "") or first_text("h1") or first_text("title") or "Untitled article")
    json_author = structured.get("author", "")
    if not isinstance(json_author, list):
        json_author = [json_author]
    author_names = [a.get("name", "") if isinstance(a, dict) else a for a in json_author]
    author = clean_text(author or meta.get("author") or ", ".join(a for a in author_names if isinstance(a, str) and a)
                        or next((n.text() for n in document.nodes if n.attrs.get("rel") == "author" or n.attrs.get("itemprop") == "author"), ""))
    date = clean_text(date or meta.get("article:published_time") or str(structured.get("datePublished") or "")
                      or next((n.attrs.get("datetime", "") for n in document.nodes if n.tag == "time"), ""))
    canonical = next((n.attrs.get("href", "") for n in document.nodes if n.tag == "link" and "canonical" in n.attrs.get("rel", "").split()), "")
    source_candidate = canonical or meta.get("og:url") or source_url
    if source_candidate:
        try:
            source_url = http_url(urljoin(source_url, source_candidate))
        except ReaderError:
            source_url = http_url(source_url) if source_url else ""
    language = next((n.attrs.get("lang", "") for n in document.nodes if n.tag == "html"), "") or "en"
    language = language if re.fullmatch(r"[a-zA-Z]{2,8}(?:-[a-zA-Z0-9]{1,8})*", language) else "en"

    # Score only visible prose, not embedded state or link-heavy menus.
    stats: dict[Node, tuple[int, int, int]] = {}
    reachable: list[Node] = []
    pending = [document.root]
    while pending:
        node = pending.pop()
        if excluded(node):
            continue
        reachable.append(node)
        pending.extend(c for c in node.children if isinstance(c, Node))
    for node in reversed(reachable):
        length = links = paragraphs = 0
        for child in node.children:
            if isinstance(child, str):
                length += len(clean_text(child))
            elif child in stats:
                size, linked, paras = stats[child]
                length += size
                links += linked
                paragraphs += paras
        if node.tag == "a":
            links = length
        if node.tag == "p" and length > 30:
            paragraphs += 1
        stats[node] = length, links, paragraphs
    candidates = []
    for node in reachable:
        length, links, paragraphs = stats[node]
        if node.tag not in {"article", "main", "div", "section", "body", "document"} or not length:
            continue
        classes = set(node.attrs.get("class", "").split())
        explicit = bool(classes & MAIN_CLASSES) or {"body", "markup"} <= classes or node.attrs.get("itemprop") == "articleBody"
        priority = 3 if explicit else 2 if node.tag == "article" else 1 if node.tag == "main" else 0
        score = length * (1 - links / length) ** 2 + paragraphs * 100
        candidates.append((priority, score, -len(node.children), node))
    if not candidates:
        raise ReaderError("No readable article content found; save the full rendered article as HTML, not a login page")
    main = max(candidates, key=lambda entry: entry[:3])[3]
    images: list[str] = []
    warnings: list[str] = []
    if any("paywall" in n.attrs.get("class", "").lower() for n in document.nodes) or structured.get("isAccessibleForFree") in (False, "False", "false"):
        warnings.append("This page may contain only a paywalled preview. Use a locally saved full HTML/export for the complete post.")

    def render(node: Node | str) -> str:
        if isinstance(node, str):
            return html.escape(xml_text(node))
        if excluded(node):
            return ""
        if node.tag == "h1" and clean_text(node.text()) == title:
            return ""
        if node.tag == "img":
            src = node.attrs.get("data-src") or node.attrs.get("data-original") or node.attrs.get("src", "")
            if not src:
                src = node.attrs.get("srcset", "").split(",")[0].strip().split(" ")[0]
            alt = html.escape(clean_text(node.attrs.get("alt", "")), quote=True)
            if not src or len(images) >= MAX_IMAGES:
                return f"<p>{alt}</p>" if alt else ""
            if src not in images:
                images.append(src)
            return f'<img src="image:{images.index(src)}" alt="{alt}" />'
        content = "".join(render(c) for c in node.children)
        if node.tag not in ALLOWED:
            return content
        tag = "h2" if node.tag == "h1" else node.tag
        anchor = node.attrs.get("id") or (node.attrs.get("name") if tag == "a" else "")
        attrs = f' id="note-{digest(anchor)[:20]}"' if anchor else ""
        if tag == "a":
            href = node.attrs.get("href", "")
            try:
                if href.startswith("#") and len(href) > 1:
                    href = "#note-" + digest(unquote(href[1:]))[:20]
                elif href:
                    full = urljoin(source_url, href)
                    fragment = urlsplit(full).fragment
                    href = http_url(full) + ("#" + fragment if fragment else "")
            except ReaderError:
                href = ""
            if href:
                attrs += f' href="{html.escape(href, quote=True)}"'
            elif not anchor:
                return content
        if tag in {"td", "th"}:
            for key in ("colspan", "rowspan"):
                value = node.attrs.get(key, "")
                if value.isdigit() and 1 <= int(value) <= 100:
                    attrs += f' {key}="{value}"'
        if tag in VOID:
            return f"<{tag}{attrs} />"
        return f"<{tag}{attrs}>{content}</{tag}>"

    content = render(main).strip()
    if not clean_text(re.sub(r"<[^>]+>", "", content)):
        raise ReaderError("Article contains no readable text; a JavaScript/login page may have been saved")
    return Article(title, author, source_url, date, language, content, images, warnings)
