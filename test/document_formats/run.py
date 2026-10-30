#!/usr/bin/env python3
"""Host integration checks for real converters, using filesystem-backed HAL adapters.

Run from the repository root: python3 test/document_formats/run.py
Requires a C++20 compiler and Expat development headers/library. No device/network.
"""
import base64
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zipfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
NS = {"x": "http://www.w3.org/1999/xhtml", "dc": "http://purl.org/dc/elements/1.1/",
      "n": "http://www.daisy.org/z3986/2005/ncx/"}
PNG = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jf1sAAAAASUVORK5CYII=")


def inspect(path):
    with zipfile.ZipFile(path) as archive:
        assert archive.testzip() is None, "ZIP CRC failed"
        assert archive.namelist()[0] == "mimetype"
        assert archive.read("mimetype") == b"application/epub+zip"
        assert archive.getinfo("mimetype").compress_type == zipfile.ZIP_STORED
        assert len(archive.namelist()) == len(set(archive.namelist())), "duplicate ZIP entries"
        body = ET.fromstring(archive.read("book.xhtml"))
        metadata = ET.fromstring(archive.read("content.opf"))
        toc = ET.fromstring(archive.read("toc.ncx"))
        ET.fromstring(archive.read("META-INF/container.xml"))
        ids = {node.attrib["id"] for node in body.iter() if "id" in node.attrib}
        for item in toc.findall(".//n:content", NS):
            href = item.attrib["src"]
            assert href.startswith("book.xhtml")
            if "#" in href:
                assert href.split("#", 1)[1] in ids, href
        for image in body.findall(".//x:img", NS):
            assert image.attrib["src"] in archive.namelist()
    return body, metadata, toc


def reject(convert, path):
    try:
        convert(path)
    except subprocess.CalledProcessError:
        return
    raise AssertionError(f"invalid document accepted: {path}")


def main():
    with tempfile.TemporaryDirectory(prefix="crosspoint-doc-formats-") as temporary:
        root = Path(temporary)
        binary = root / "convert"
        sources = [HERE / "convert.cpp", REPO / "lib/Markdown/ConvertedEpub.cpp",
                   REPO / "lib/Markdown/Markdown.cpp", REPO / "lib/Fb2/Fb2.cpp",
                   REPO / "lib/Epub/Epub/htmlEntities.cpp"]
        command = shlex.split(os.environ.get("CXX", "c++")) + ["-std=c++20", "-Wall", "-Wextra", "-g"]
        if os.environ.get("SANITIZE", "1") != "0":
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        for include in (HERE / "stubs", REPO / "lib/Markdown", REPO / "lib/Fb2", REPO / "lib/Epub"):
            command += ["-I", str(include)]
        html = REPO / "lib/Html/Html.cpp"
        if html.exists():
            command += ["-DDOCUMENT_FORMAT_HTML", "-I", str(REPO / "lib/Html")]
            sources += [html]
        command += shlex.split(os.environ.get("CPPFLAGS", ""))
        command += [str(source) for source in sources] + ["-lexpat"]
        command += shlex.split(os.environ.get("LDFLAGS", "")) + ["-o", str(binary)]
        print("Building:", shlex.join(command), flush=True)
        subprocess.run(command, check=True)
        cache = root / ".crosspoint"

        def convert(path, extra_env=None):
            env = os.environ.copy()
            if extra_env:
                env.update(extra_env)
            result = subprocess.run([str(binary), str(path), str(cache)], check=False,
                                    capture_output=True, text=True, env=env)
            if result.returncode:
                print(result.stderr, end="", flush=True)
                result.check_returncode()
            return Path(result.stdout.strip())

        md = root / "sample.md"
        (root / "pixel.png").write_bytes(PNG)
        md.write_text("""---
title: 'A & B'
author: Test Author
language: en
---
# Opening
A **bold** and *italic* paragraph with `code <&>` and [link](#opening).
Soft wrapped
line with a hard break.  
Next line.

- first
- second

1. numbered
2. another

> Quoted **words**.

```cpp
  if (a < b) {
    print("<&>");
  }
```

## Table
| Name | Value |
| --- | --- |
| Alpha | **Beta** |

![Pixel](pixel.png)
![Remote fallback](https://example.invalid/p.png)

Setext Heading
==============
Escaped \\*star\\* and unmatched *delimiter remain visible.
""", encoding="utf-8")
        archive = convert(md)
        body, meta, toc = inspect(archive)
        assert meta.find(".//dc:title", NS).text == "A & B"
        assert meta.find(".//dc:creator", NS).text == "Test Author"
        assert meta.find(".//dc:language", NS).text == "en"
        assert body.find(".//x:strong", NS).text == "bold"
        assert body.find(".//x:em", NS).text == "italic"
        assert len(body.findall(".//x:li", NS)) == 4
        assert len(body.findall(".//x:table/x:tr", NS)) == 2
        assert len(body.findall(".//x:blockquote", NS)) == 1
        assert len(body.findall(".//x:img", NS)) == 1
        assert len(toc.findall(".//n:navPoint", NS)) == 3
        text = "".join(body.itertext())
        assert "Remote fallback" in text and "*star*" in text and "*delimiter" in text
        assert "\u00a0\u00a0if" in text and 'print("<&>");' in text
        with zipfile.ZipFile(archive) as zip_file:
            assert zip_file.read(body.find(".//x:img", NS).attrib["src"]) == PNG

        # Cache reuse and a content edit with unchanged size and mtime.
        before = archive.stat().st_mtime_ns
        marker = archive.parent / "converted.meta"
        before_marker = marker.read_bytes()
        assert convert(md) == archive and archive.stat().st_mtime_ns == before
        progress = archive.parent / "progress.bin"
        bookmarks = archive.parent / "bookmarks.bin"
        progress.write_bytes(b"keep-progress")
        bookmarks.write_bytes(b"keep-bookmarks")
        (archive.parent / "book.bin").write_bytes(b"stale")
        sections = archive.parent / "sections"
        sections.mkdir()
        (sections / "0.bin").write_bytes(b"stale")
        old_stat = md.stat()
        md.write_text(md.read_text().replace("Alpha", "Omega"))
        os.utime(md, ns=(old_stat.st_atime_ns, old_stat.st_mtime_ns))
        assert convert(md) == archive
        assert marker.read_bytes() != before_marker
        assert not sections.exists() and not (archive.parent / "book.bin").exists()
        assert progress.read_bytes() == b"keep-progress" and bookmarks.read_bytes() == b"keep-bookmarks"
        assert "Omega" in "".join(inspect(archive)[0].itertext())
        last_marker = marker.read_bytes()
        stamp = md.stat()
        os.utime(md, (stamp.st_atime, stamp.st_mtime + 4))
        convert(md)
        assert marker.read_bytes() != last_marker

        # Truncated output and invalid marker rebuild rather than returning a bad archive.
        archive.write_bytes(b"broken")
        inspect(convert(md))
        marker.write_bytes(b"broken")
        inspect(convert(md))

        long_md = root / "long.markdown"
        long_text = "a" * 2047 + "界" * 9000 + " ending"
        long_md.write_text("# Boundary\r\n\r\n" + long_text, encoding="utf-8")
        long_body = inspect(convert(long_md))[0]
        assert long_text in "".join(long_body.itertext())
        long_md.write_text("```\n" + "x" * 4096, encoding="utf-8")
        assert "x" * 4096 in "".join(inspect(convert(long_md))[0].itertext())
        long_md.write_bytes(b"\xef\xbb\xbf# BOM\rSecond\r\rThird\r")
        assert inspect(convert(long_md))[1].find(".//dc:title", NS).text == "BOM"
        long_md.write_text("---\nNot frontmatter without a terminator\n" + "x" * 10000)
        assert "Not frontmatter" in "".join(inspect(convert(long_md))[0].itertext())
        long_md.write_bytes(b"# Repair\n\nA\x00B\xffC\n")
        repaired = "".join(inspect(convert(long_md))[0].itertext())
        assert "A\ufffdB\ufffdC" in repaired

        fb = root / "sample.fb2"
        fb.write_text('''<?xml version="1.0" encoding="UTF-8"?>
<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0" xmlns:l="http://www.w3.org/1999/xlink">
<description><title-info><book-title>Fiction &amp; Book</book-title>
<author><first-name>Jane</first-name><middle-name>Q.</middle-name><last-name>Writer</last-name></author>
<lang>en</lang></title-info><document-info><author><first-name>Wrong</first-name></author></document-info></description>
<body><section id="chapter"><title><p>First Chapter</p></title>
<p>A <strong>bold</strong> and <emphasis>italic</emphasis> paragraph.</p>
<poem><stanza><v>A verse</v><v>Another verse</v></stanza></poem>
<cite><p>A quotation</p></cite><empty-line/>
<p>See <a type="note" l:href="#note">1</a>.</p><image l:href="#pixel"/>
<table><tr><th>A</th><th>B</th></tr><tr><td>1</td><td>2</td></tr></table>
</section></body>
<body name="notes"><section id="note"><title><p>Notes</p></title><p>Footnote text</p></section></body>
<binary id="pixel" content-type="image/png">''' + base64.b64encode(PNG).decode() + '''</binary></FictionBook>''', encoding="utf-8")
        fb_archive = convert(fb)
        fb_body, fb_meta, fb_toc = inspect(fb_archive)
        assert fb_meta.find(".//dc:title", NS).text == "Fiction & Book"
        assert fb_meta.find(".//dc:creator", NS).text == "Jane Q. Writer"
        assert len(fb_toc.findall(".//n:navPoint", NS)) == 2
        assert fb_body.find(".//x:strong", NS).text == "bold"
        assert fb_body.find(".//x:em", NS).text == "italic"
        assert "Footnote text" in "".join(fb_body.itertext())
        assert fb_body.find(".//x:a", NS).attrib["href"] == "#note"
        with zipfile.ZipFile(fb_archive) as zip_file:
            assert zip_file.read(fb_body.find(".//x:img", NS).attrib["src"]) == PNG

        encoded = root / "encoded.fb2"
        template = '<?xml version="1.0" encoding="{}"?><FictionBook><description><title-info><book-title>Привет</book-title></title-info></description><body><section><p>Чтение книги</p></section></body></FictionBook>'
        for encoding in ("windows-1251", "koi8-r", "utf-16", "utf-8"):
            encoded.write_bytes(template.format(encoding).encode(encoding))
            converted = inspect(convert(encoded))
            assert converted[1].find(".//dc:title", NS).text == "Привет", encoding
            assert "Чтение книги" in "".join(converted[0].itertext()), encoding

        malformed = root / "malformed.fb2"
        bad_documents = ["<FictionBook><body><p>broken</body></FictionBook>",
                         "<html><body>not FB2</body></html>",
                         '<!DOCTYPE FictionBook [<!ENTITY x SYSTEM "file:///etc/passwd">]><FictionBook><body><p>&x;</p></body></FictionBook>',
                         "<FictionBook><body>" + "<section>" * 65 + "x" + "</section>" * 65 + "</body></FictionBook>",
                         '<FictionBook><body><p x="' + "a" * 100000 + '">x</p></body></FictionBook>',
                         '<FictionBook><body><p>x</p></body><binary id="x" content-type="image/png">a===</binary></FictionBook>']
        for document in bad_documents:
            malformed.write_text(document)
            reject(convert, malformed)
        assert not list(cache.glob("*/convert.*.tmp"))
        assert not list(cache.glob("*/converted.epub.images.tmp"))

        # An I/O failure must not overwrite the previously usable archive.
        original = fb_archive.read_bytes()
        fb.write_text(fb.read_text().replace("First Chapter", "Changed Chapter"))
        reject(lambda path: convert(path, {"FORMAT_FAIL_WRITE_AFTER": "100"}), fb)
        assert fb_archive.read_bytes() == original
        assert not list(cache.glob("*/convert.*.tmp"))
        inspect(convert(fb))

        html_cases = HERE / "html_cases.py"
        if html.exists() and html_cases.exists():
            spec = importlib.util.spec_from_file_location("html_cases", html_cases)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            module.run(convert, root)
        print("Document-format integration checks passed (host HAL; no firmware/device checks).")


if __name__ == "__main__":
    main()
