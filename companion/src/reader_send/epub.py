from __future__ import annotations

import base64
import binascii
import html
import io
import re
import warnings
import zipfile
from datetime import datetime
from urllib.parse import unquote_to_bytes, urljoin, urlsplit
from xml.etree import ElementTree

from PIL import Image, ImageOps, UnidentifiedImageError

from .common import MAX_IMAGE_BYTES, ReaderError, digest, http_url
from .extract import Article
from .network import PublicHTTP
from .sources import LocalSource

MAX_PIXELS = 20_000_000
MAX_EDGE = 1200
MAX_PACKED_IMAGE = 512 * 1024


def resize_image(data: bytes) -> bytes:
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", Image.DecompressionBombWarning)
            with Image.open(io.BytesIO(data)) as original:
                if original.width * original.height > MAX_PIXELS:
                    raise ReaderError("Image exceeds 20 megapixels")
                if original.width <= 1 or original.height <= 1:
                    raise ReaderError("Tracking pixel omitted")
                image = ImageOps.exif_transpose(original)
                image.thumbnail((MAX_EDGE, MAX_EDGE), Image.Resampling.LANCZOS)
                if image.mode in ("RGBA", "LA", "P"):
                    rgba = image.convert("RGBA")
                    image = Image.new("RGB", rgba.size, "white")
                    image.paste(rgba, mask=rgba.getchannel("A"))
                else:
                    image = image.convert("RGB")
                for quality in (85, 70, 50):
                    target = io.BytesIO()
                    image.save(target, "JPEG", quality=quality, optimize=True)
                    if target.tell() <= MAX_PACKED_IMAGE:
                        return target.getvalue()
                while image.width > 1 and image.height > 1:
                    image.thumbnail((max(1, image.width * 3 // 4), max(1, image.height * 3 // 4)))
                    target = io.BytesIO()
                    image.save(target, "JPEG", quality=50, optimize=True)
                    if target.tell() <= MAX_PACKED_IMAGE:
                        return target.getvalue()
    except (UnidentifiedImageError, OSError, ValueError, Image.DecompressionBombError, Image.DecompressionBombWarning) as exc:
        raise ReaderError(f"Unsupported or unsafe image: {exc}") from exc
    raise ReaderError("Cannot fit image within 512 KiB")


def image_bytes(reference: str, article: Article, local: LocalSource | None, client: PublicHTTP, remote: bool) -> bytes:
    if reference.startswith("data:"):
        header, separator, body = reference.partition(",")
        if not separator or not header.startswith("data:image/") or len(body) > MAX_IMAGE_BYTES * 2:
            raise ReaderError("Invalid or oversized embedded image")
        try:
            result = base64.b64decode(body, validate=True) if ";base64" in header else unquote_to_bytes(body)
        except (binascii.Error, ValueError) as exc:
            raise ReaderError("Malformed embedded image") from exc
        if len(result) > MAX_IMAGE_BYTES:
            raise ReaderError("Embedded image exceeds 8 MiB")
        return result
    parts = urlsplit(reference)
    if local and not parts.scheme and not parts.netloc:
        return local.image(reference)
    if not remote:
        raise ReaderError("Remote image skipped (use --remote-images for locally saved HTML)")
    url = http_url(urljoin(article.source_url, reference))
    return client.get(url, MAX_IMAGE_BYTES).data


def build_epub(article: Article, identity: str, client: PublicHTTP, *, local: LocalSource | None = None,
               remote_images: bool = True, no_images: bool = False) -> bytes:
    packed: dict[int, bytes] = {}
    if not no_images:
        for index, reference in enumerate(article.images):
            try:
                packed[index] = resize_image(image_bytes(reference, article, local, client, remote_images))
            except (ReaderError, OSError, ValueError, zipfile.BadZipFile) as exc:
                # Keep prose and image alt text even if an optional illustration fails.
                article.warnings.append(f"Image {index + 1} omitted: {exc}")
    def image_tag(match):
        index = int(match.group(1))
        alt = match.group(2)
        if index in packed:
            return f'<img src="images/{index}.jpg" alt="{alt}" />'
        return f'<span class="image-alt">{alt}</span>' if alt else ""
    content = re.sub(r'<img src="image:(\d+)" alt="([^"]*)" />', image_tag, article.content)
    esc = html.escape
    byline = f"<p>{esc(article.author)}</p>" if article.author else ""
    published = f"<p>{esc(article.date)}</p>" if article.date else ""
    source = f'<p>Source: <a href="{esc(article.source_url, quote=True)}">{esc(article.source_url)}</a></p>' if article.source_url else ""
    xhtml = f'''<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="{article.language}"><head><title>{esc(article.title)}</title>
<link rel="stylesheet" type="text/css" href="style.css" /></head><body><h1>{esc(article.title)}</h1>
{byline}{published}{source}<div>{content}</div></body></html>'''
    try:
        ElementTree.fromstring(xhtml)
    except ElementTree.ParseError as exc:
        raise ReaderError(f"Cannot produce valid article XHTML: {exc}") from exc
    uid = "urn:sha256:" + digest(identity)
    creator = f"<dc:creator>{esc(article.author)}</dc:creator>" if article.author else ""
    source_meta = f"<dc:source>{esc(article.source_url)}</dc:source>" if article.source_url else ""
    date_meta = ""
    if article.date:
        try:
            date_meta = f"<dc:date>{datetime.fromisoformat(article.date.replace('Z', '+00:00')).date().isoformat()}</dc:date>"
        except ValueError:
            pass  # Keep non-ISO publication labels in the visible article header.
    image_manifest = "".join(f'<item id="image-{i}" href="images/{i}.jpg" media-type="image/jpeg"/>' for i in packed)
    opf = f'''<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="book-id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="book-id">{uid}</dc:identifier>
<dc:title>{esc(article.title)}</dc:title><dc:language>{article.language}</dc:language>{creator}{source_meta}{date_meta}</metadata>
<manifest><item id="article" href="article.xhtml" media-type="application/xhtml+xml"/>
<item id="style" href="style.css" media-type="text/css"/><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>{image_manifest}</manifest>
<spine toc="ncx"><itemref idref="article"/></spine></package>'''
    ncx = f'''<?xml version="1.0" encoding="UTF-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head><meta name="dtb:uid" content="{uid}"/>
<meta name="dtb:depth" content="1"/><meta name="dtb:totalPageCount" content="0"/><meta name="dtb:maxPageNumber" content="0"/></head>
<docTitle><text>{esc(article.title)}</text></docTitle><navMap><navPoint id="article" playOrder="1"><navLabel><text>{esc(article.title)}</text></navLabel>
<content src="article.xhtml"/></navPoint></navMap></ncx>'''
    entries: dict[str, bytes | str] = {
        "mimetype": "application/epub+zip",
        "META-INF/container.xml": '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>',
        "OEBPS/content.opf": opf, "OEBPS/toc.ncx": ncx, "OEBPS/article.xhtml": xhtml,
        "OEBPS/style.css": "body { line-height: 1.4; } img { max-width: 100%; height: auto; } pre { white-space: pre-wrap; } blockquote { margin: 1em; } .image-alt { font-style: italic; }",
    }
    entries.update({f"OEBPS/images/{i}.jpg": data for i, data in packed.items()})
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as archive:
        for name, data in entries.items():
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED if name == "mimetype" else zipfile.ZIP_DEFLATED
            info.external_attr = 0o600 << 16
            archive.writestr(info, data.encode("utf-8") if isinstance(data, str) else data)
    return output.getvalue()
