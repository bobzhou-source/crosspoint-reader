"""HTML-only cases for the shared document-format host conversion harness."""

import base64
import os
import xml.etree.ElementTree as ET
import zipfile


XHTML = "{http://www.w3.org/1999/xhtml}"
DC = "{http://purl.org/dc/elements/1.1/}"
NCX = "{http://www.daisy.org/z3986/2005/ncx/}"
PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Wl6"
    "SAAAAABJRU5ErkJggg=="
)


def inspect(path):
    with zipfile.ZipFile(path) as archive:
        assert archive.testzip() is None
        first = archive.infolist()[0]
        assert first.filename == "mimetype"
        assert first.compress_type == zipfile.ZIP_STORED
        assert archive.read("mimetype") == b"application/epub+zip"
        names = archive.namelist()
        for name in names:
            assert not name.startswith("/") and ".." not in name.split("/")
            if name.endswith((".xml", ".xhtml", ".opf", ".ncx")):
                ET.fromstring(archive.read(name))
        body = ET.fromstring(archive.read("book.xhtml")).find(XHTML + "body")
        opf = ET.fromstring(archive.read("content.opf"))
        toc_name = next(name for name in names if name.endswith(".ncx"))
        toc = ET.fromstring(archive.read(toc_name))
        images = {name: archive.read(name) for name in names if name.startswith("images/")}
        assert body is not None
        return body, opf, toc, images


def expect_failure(convert, source):
    try:
        convert(source)
    except Exception:
        return
    raise AssertionError(f"Conversion unexpectedly accepted {source.name}")


def run(convert, root):
    directory = root / "html"
    directory.mkdir()
    (directory / "assets").mkdir()
    (directory / "assets" / "tiny image.png").write_bytes(PNG)
    source = directory / "reading.html"
    source.write_text(
        """<!DOCTYPE HTML>
<HTML lang=en><HEAD><meta charset=UTF-8>
<TITLE>Fish &amp; Chips &#x1f41f;</TITLE>
<meta NAME=author CONTENT='Ada &amp; Bob'>
<style>STYLE_MUST_NOT_APPEAR</style>
<script>if (2 < 3) { document.write('<p>SCRIPT_MUST_NOT_APPEAR'); }</script>
</HEAD><BODY><H1 id=start>Chapter <em>one</em></H1>
<p>First &copy; &nbsp; &unknown; & bare <b>bold</b>
<p>Second 2 < 3 and &#0; &#xD800; &#x110000;.
<ul><li>Alpha<li>Beta<ul><li>Nested</ul><li>Gamma</ul>
<dl><dt>Term<dd>Definition</dl>
<table><tr><td>One<td>Two<tr><td>Three</table>
<h2>Chapter two</h2><a href='#start'>Back</a>
<pre>line one
line two</pre>
<img src='assets/tiny%20image.png?version=1#ignored' alt='Local &amp; image'>
<img src='https://example.invalid/remote.png' alt='REMOTE_ALT'>
<img src='//example.invalid/remote.png' alt='PROTOCOL_ALT'>
<img src='data:image/png;base64,aaaa' alt='DATA_ALT'>
<img src='http%3a%2f%2fexample.invalid/a.png' alt='ENCODED_ALT'>
<img src='../../../../../../outside.png' alt='TRAVERSAL_ALT'>
<img src='missing.png' alt='MISSING_ALT'>
<iframe>FRAME_MUST_NOT_APPEAR</iframe>
<!-- COMMENT_MUST_NOT_APPEAR <p> -->
<p id='quote"&amp;&lt;id'>Safe anchor</p>
</BODY></HTML>""",
        encoding="utf-8",
    )
    archive = convert(source)
    body, opf, toc, images = inspect(archive)
    text = "".join(body.itertext())
    assert opf.find(".//" + DC + "title").text == "Fish & Chips 🐟"
    assert opf.find(".//" + DC + "creator").text == "Ada & Bob"
    assert "First ©" in text and "&unknown; & bare" in text
    assert "Second 2 < 3 and � � �." in text
    assert "AlphaBetaNestedGamma" in text
    assert "OneTwoThree" in text and "TermDefinition" in text
    for omitted in ("STYLE", "SCRIPT", "FRAME", "COMMENT"):
        assert omitted + "_MUST_NOT_APPEAR" not in text
    for fallback in ("REMOTE_ALT", "PROTOCOL_ALT", "DATA_ALT", "ENCODED_ALT", "TRAVERSAL_ALT", "MISSING_ALT"):
        assert fallback in text
    assert list(images.values()) == [PNG]
    image = body.find(".//" + XHTML + "img")
    assert image is not None and image.attrib["src"] in images
    assert image.attrib["alt"] == "Local & image"
    anchors = {node.attrib["id"] for node in body.iter() if "id" in node.attrib}
    assert 'quote"&<id' in anchors
    entries = toc.findall(".//" + NCX + "navPoint")
    assert len(entries) == 2
    assert [entry.find(".//" + NCX + "text").text for entry in entries] == ["Chapter one", "Chapter two"]
    for entry in entries:
        href = entry.find(NCX + "content").attrib["src"]
        assert href.split("#", 1)[1] in anchors
    assert body.find(".//" + XHTML + "br") is not None

    # Cache hits must not rewrite the archive; mtime and same-sized edits both invalidate it.
    initial_mtime = archive.stat().st_mtime_ns
    marker_path = archive.with_name("converted.meta")
    initial_marker = marker_path.read_bytes()
    assert convert(source) == archive
    assert archive.stat().st_mtime_ns == initial_mtime
    source_stat = source.stat()
    os.utime(source, ns=(source_stat.st_atime_ns, source_stat.st_mtime_ns + 2_000_000_000))
    assert convert(source) == archive
    assert marker_path.read_bytes() != initial_marker
    touched_marker = marker_path.read_bytes()
    source_stat = source.stat()
    source.write_text(source.read_text(encoding="utf-8").replace("First", "Later"), encoding="utf-8")
    os.utime(source, ns=(source_stat.st_atime_ns, source_stat.st_mtime_ns))
    assert convert(source) == archive
    assert marker_path.read_bytes() != touched_marker
    assert "Later ©" in "".join(inspect(archive)[0].itertext())

    # Arbitrary read-buffer boundaries must not split UTF-8, entity or attribute tokens.
    for padding in (1014, 1015, 1016, 1017, 1022, 1023, 1024):
        boundary = directory / f"boundary-{padding}.htm"
        boundary.write_bytes(b"<p>" + b"x" * padding + "雪🐟&amp;&#x1f41f;</p>".encode())
        actual = "".join(inspect(convert(boundary))[0].itertext())
        assert actual == "x" * padding + "雪🐟&🐟"

    invalid = directory / "invalid-bytes.html"
    invalid.write_bytes(b"<p>A\x00\x01\xc0\xaf\xed\xa0\x80\xf4\x90\x80\x80B</p>")
    invalid_text = "".join(inspect(convert(invalid))[0].itertext())
    assert invalid_text.startswith("A") and invalid_text.endswith("B") and "�" in invalid_text

    latin = directory / "latin.htm"
    latin.write_bytes(b"<meta charset=windows-1252><title>Caf\xe9</title><p>\x93Caf\xe9\x94 \x80</p>")
    latin_body, latin_opf, _, _ = inspect(convert(latin))
    assert latin_opf.find(".//" + DC + "title").text == "Café"
    assert "".join(latin_body.itertext()) == "“Café” €"

    broken = directory / "repair.html"
    broken.write_text("<div><p>A<b>B<i>C</b>D</p><p>E<br>F<img src=missing alt=ALT></div>", encoding="utf-8")
    assert "ABCDE" in "".join(inspect(convert(broken))[0].itertext())

    deep = directory / "deep.html"
    deep.write_text("<div>" * 1000 + "DEPTH_TEXT" + "</div>" * 1000, encoding="utf-8")
    assert "".join(inspect(convert(deep))[0].itertext()) == "DEPTH_TEXT"

    oversized = directory / "oversized.html"
    oversized.write_text(
        "<title>" + "雪" * 1000 + "</title><p data-large='" + "x" * 20000 + "'>LONG_TAG_TEXT</p>"
        + "<p>" + "streamed words " * 100000 + "END</p>",
        encoding="utf-8",
    )
    large_body, large_opf, _, _ = inspect(convert(oversized))
    assert len(large_opf.find(".//" + DC + "title").text.encode()) <= 255
    large_text = "".join(large_body.itertext())
    assert large_text.startswith("LONG_TAG_TEXT") and large_text.endswith("END")
    assert large_text.count("streamed words ") == 100000

    many = directory / "many-headings.html"
    many.write_text("".join(f"<h2>Heading {i}</h2><p>Text {i}</p>" for i in range(300)), encoding="utf-8")
    many_body, _, many_toc, _ = inspect(convert(many))
    assert len(many_toc.findall(".//" + NCX + "navPoint")) == 256
    assert "Heading 299Text 299" in "".join(many_body.itertext())

    image_limit = directory / "image-limit.html"
    image_limit.write_text(
        "".join(f"<img src='assets/tiny%20image.png' alt='Image {i}'>" for i in range(129)), encoding="utf-8"
    )
    limit_body, _, _, limit_images = inspect(convert(image_limit))
    assert len(limit_images) == 128 and "Image 128" in "".join(limit_body.itertext())

    # A failed reconversion must not publish a partial archive or retain conversion temporaries.
    previous_archive = archive.read_bytes()
    source.write_text("<meta charset=unsupported-charset><p>Not a usable encoding</p>", encoding="utf-8")
    expect_failure(convert, source)
    assert archive.read_bytes() == previous_archive
    assert not list(archive.parent.glob("convert.*.tmp"))
    utf16 = directory / "utf16.html"
    utf16.write_bytes("<p>UTF-16 requires explicit transcoding</p>".encode("utf-16"))
    expect_failure(convert, utf16)
