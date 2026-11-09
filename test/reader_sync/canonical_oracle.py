#!/usr/bin/env python3
"""Cross-language proof for src/network/ReaderSyncCanonical.cpp.

Compiles the production canonicalizer twice against the repo's ArduinoJson
(shortest-digits float path and printf/strtod float path), feeds it native
annotation records written by ArduinoJson's own writer plus adversarial raw JSON,
and compares every result with the companion's own
reader_send.cloud_annotations.map_annotation()/cloud.json_bytes().

Run from anywhere:

    python3 test/reader_sync/canonical_oracle.py

Requires a C++17 compiler, the repo's .pio ArduinoJson copy (pio pkg install -e
x4pro-gh_release), and only Python's standard library.
"""
from __future__ import annotations

import base64
import json
import math
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
ARDUINOJSON = ROOT / ".pio/libdeps/x4pro-gh_release/ArduinoJson/src"
SOURCE = ROOT / "src/network"
DEVICE = "00000000-0000-0000-0000-000000000000"
NOW = 1767225600  # 2026-01-01T00:00:00Z

sys.path.insert(0, str(ROOT / "companion/src"))
from reader_send.cloud import json_bytes, parse_json  # noqa: E402
from reader_send.cloud_annotations import map_annotation  # noqa: E402
from reader_send.common import ReaderError  # noqa: E402

BASE = [
    ("kind", "0"),
    ("version", "1"),
    ("id", "7"),
    ("sourcePath", '"Books/a.epub"'),
    ("sourceTitle", '"T"'),
    ("chapterTitle", '"C"'),
    ("chapterHref", '"c.xhtml"'),
    ("xpath", '"/body/p[1]"'),
    ("percentage", "0.5"),
    ("si", "1"),
    ("pc", "2"),
    ("pp", "1"),
    ("excerpt", '"hi"'),
    ("note", '"n"'),
    ("pageContext", "true"),
    ("createdAt", str(NOW)),
]


def native(override=None, drop=(), add=()):
    pending = dict(override or {})
    items = []
    for key, value in BASE:
        if key in drop:
            continue
        items.append((key, pending.pop(key, value)))
    items.extend(pending.items())
    items.extend(add)
    return ("{" + ",".join(f'"{k}":{v}' for k, v in items) + "}").encode()


def case(name, raw, device=DEVICE, now=NOW):
    return {"name": name, "raw": raw, "device": device, "now": now}


def cases():
    out = [
        case("base", native()),
        case("vo-absent", native()),
        case("vo-zero", native({"vo": "0"})),
        case("vo-value", native({"vo": "65535"})),
        case("kind-0", native({"kind": "0"})),
        case("kind-1", native({"kind": "1"})),
        case("kind-2", native({"kind": "2"})),
        case("kind-name", native({"kind": '"Question"'})),
        case("kind-prefixed", native({"kind": '"AnnotationKind::ReviewLater"'})),
        case("kind-lower", native({"kind": '"review_later"'})),
        case("kind-missing", native(drop=("kind",))),
        case("kind-3", native({"kind": "3"})),
        case("kind-float", native({"kind": "1.0"})),
        case("kind-bool", native({"kind": "true"})),
        case("kind-bogus", native({"kind": '"bogus"'})),
        case("kind-null", native({"kind": "null"})),
        case("percent-float", native({"percentage": "0.423700005"})),
        case("percent-tiny", native({"percentage": "1e-6"})),
        case("percent-1e-4", native({"percentage": "0.0001"})),
        case("percent-1e-5", native({"percentage": "0.00001"})),
        case("percent-100", native({"percentage": "100.0"})),
        case("percent-negative-zero", native({"percentage": "-0.0"})),
        case("percent-1e16", native({"percentage": "1e16"})),
        case("percent-1e15", native({"percentage": "1e15"})),
        case("percent-denormal", native({"percentage": "5e-324"})),
        case("percent-max", native({"percentage": "1.7976931348623157e308"})),
        case("percent-underflow", native({"percentage": "1e-400"})),
        case("percent-tenth", native({"percentage": "0.1"})),
        case("percent-pi", native({"percentage": "3.14159265"})),
        case("pc-float", native({"pc": "10.0", "pp": "9.5", "si": "1.0"})),
        case("unknown-nested", native(add=(('"extras"', '{"zeta":1,"alpha":"x","list":[1,2.5,true,null]}'),))),
        case("unknown-empty-object", native(add=(('"extras"', "{}"),))),
        case("unknown-empty-array", native(add=(('"extras"', "[]"),))),
        case("cjk", native({"sourceTitle": '"书名 📚 café"', "chapterTitle": '"第一章"', "note": '"备注 🎉"'})),
        case("escapes", native({"excerpt": '"a\\"b\\\\c\\n\\t\\u0001d"'})),
        case("escaped-unicode", native({"excerpt": '"\\u00e9 \\u4e2d \\ud83d\\ude00"'})),
        case("escaped-slash", native({"xpath": '"/body\\/p[1]"', "chapterHref": '"c\\/c.xhtml"'})),
        case("del-char", native({"note": '"a\\u007fb"'})),
        case("nul-escape", native({"note": '"a\\u0000b"', "excerpt": '"\\u001f"'})),
        case("blank-selection", native({"excerpt": '"   "'})),
        case("multiline-note", native({"note": '"one\\ntwo\\rthree\\tfour"'})),
        case("int-huge", native(add=(('"huge"', "123456789012345678901234567890"),))),
        case("int-negative", native(add=(('"small"', "-9223372036854775808"),))),
        case("int-2pow53", native(add=(('"edge"', "9007199254740991"),))),
        case("int-digits-4300", native(add=(('"big"', "1" + "0" * 4299),))),
        case("int-digits-4301", native(add=(('"big"', "1" + "0" * 4300),))),
        case("negative-zero-int", native(add=(('"zero"', "-0"),))),
        case("created-zero", native({"createdAt": "0"})),
        case("created-max", native({"createdAt": "9007199254740"})),
        case("created-too-big", native({"createdAt": "9007199254741"})),
        case("created-future-300", native({"createdAt": str(NOW + 300)})),
        case("created-future-301", native({"createdAt": str(NOW + 301)})),
        case("created-negative", native({"createdAt": "-1"})),
        case("created-float", native({"createdAt": "1767225600.0"})),
        case("id-1", native({"id": "1"})),
        case("id-128", native({"id": "128"})),
        case("id-0", native({"id": "0"})),
        case("id-129", native({"id": "129"})),
        case("id-float", native({"id": "7.0"})),
        case("id-string", native({"id": '"7"'})),
        case("missing-id", native(drop=("id",))),
        case("missing-source-path", native(drop=("sourcePath",))),
        case("source-path-number", native({"sourcePath": "5"})),
        case("source-path-blank", native({"sourcePath": '"   "'})),
        case("source-path-control", native({"sourcePath": '"a\\u0001b"'})),
        case("note-too-long", native({"note": '"' + "n" * 8193 + '"'})),
        case("note-max", native({"note": '"' + "n" * 6000 + '"'})),
        case("anchor-too-long", native({"sourceTitle": '"' + "t" * 2100 + '"'})),
        case("duplicate-key", native(add=(('"id"', "8"),))),
        case("duplicate-escaped-key", native(add=(('"\\u0069d"', "9"),))),
        case("trailing-data", native() + b" {}"),
        case("root-array", b"[]"),
        case("root-string", b'"x"'),
        case("empty-object", b"{}"),
        case("empty", b""),
        case("nan", native({"percentage": "NaN"})),
        case("infinity", native({"percentage": "Infinity"})),
        case("negative-infinity", native({"percentage": "-Infinity"})),
        case("overflow-float", native({"percentage": "1e999"})),
        case("unpaired-surrogate", native({"note": '"\\ud800"'})),
        case("lone-low-surrogate", native({"note": '"\\udc00"'})),
        case("bad-escape", native({"note": '"a\\qb"'})),
        case("invalid-utf8", native()[:20] + b"\xff" + native()[21:]),
        case("unquoted-key", b"{id:7}"),
        case("single-quotes", b"{'id':7}"),
        case("leading-zero", native({"id": "07"})),
        case("plus-number", native({"percentage": "+1"})),
        case("upper-exponent", native({"percentage": "1E5"})),
        case("exponent-no-sign", native({"percentage": "1e5"})),
        case("percent-int", native({"percentage": "1"})),
        case("percent-string", native({"percentage": '"0.5"'})),
        case("percent-null", native({"percentage": "null"})),
        case("now-negative", native({"createdAt": "0"}), now=-1),
        case("now-huge", native({"createdAt": "0"}), now=2**62),
        case("now-zero", native({"createdAt": "0"}), now=0),
        case("trailing-comma", native()[:-1] + b",}"),
        case("too-deep", native(add=(('"deep"', "[" * 11 + "]" * 11),))),
        case("depth-limit", native(add=(('"deep"', "[" * 10 + "]" * 10),))),
        case("device-short", native(), device="card"),
        case("device-blank", native(), device="   "),
        case("device-control", native(), device="a\tb"),
        case("device-128", native(), device="d" * 128),
        case("device-129", native(), device="d" * 129),
        case("device-emoji-128", native(), device="😀" * 64),
        case("device-emoji-129", native(), device="😀" * 64 + "a"),
    ]

    # Record exactly at the 8192-byte input ceiling, and one byte over.
    base = native()
    pad = 8192 - len(base) + len('"T"')
    exact = native({"sourceTitle": '"' + "t" * (pad - 2) + '"'})
    assert len(exact) == 8192, len(exact)
    out.append(case("input-8192", exact))
    out.append(case("input-8193", b" " + exact))

    # Shortest-round-trip floats: use Python's repr as the lexeme so the double
    # is unambiguous, then require the canonicalizer to reproduce that repr.
    rng = random.Random(20261030)
    samples = [0.0, -0.0, 1.0, -1.0, 0.1, 0.5, 1e-6, 1e16, 1e-7, 123.456]
    while len(samples) < 300:
        value = struct.unpack("<d", struct.pack("<Q", rng.getrandbits(64)))[0]
        if math.isfinite(value):
            samples.append(value)
    for index, value in enumerate(samples):
        out.append(case(f"repr-{index}", native({"percentage": repr(value)})))

    return out


def expected(raw, device, now):
    """Exactly what the companion would upload, or (False, None, None)."""
    try:
        value = parse_json(raw)
        record = map_annotation(value, device_id=device, item_id=None, now=now)
        encoded = json_bytes(record)
        if len(encoded) + len(b'{"annotations":[]}') > 64 * 1024:
            return False, None, None
        return True, encoded, record["id"]
    except ReaderError:
        return False, None, None


def build(binary, cases_):
    payload = "".join(
        json.dumps(
            {"name": item["name"], "raw_b64": base64.b64encode(item["raw"]).decode("ascii"),
             "device": item["device"], "now": item["now"]},
            ensure_ascii=False,
        )
        + "\n"
        for item in cases_
    )
    proc = subprocess.run([str(binary)], input=payload, capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(f"{binary} exited {proc.returncode}: {proc.stderr}")
    results = {}
    impl = None
    for line in proc.stdout.splitlines():
        if not line.strip():
            continue
        entry = json.loads(line)
        if entry.get("kind") == "meta":
            impl = entry["float_impl"]
            continue
        results[entry["name"]] = entry
    return impl, results


def compile_smoke(output, extra):
    compiler = "c++"
    command = [
        compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-pedantic",
        "-DREADER_SYNC_HOST_TEST=1", *extra,
        f"-I{ARDUINOJSON}", f"-I{SOURCE}",
        str(HERE / "canonical_smoke.cpp"), str(SOURCE / "ReaderSyncCanonical.cpp"),
        "-o", str(output),
    ]
    subprocess.run(command, check=True)
    return command


def main():
    if not (ARDUINOJSON / "ArduinoJson.h").exists():
        raise SystemExit(f"missing ArduinoJson at {ARDUINOJSON}; run pio pkg install -e x4pro-gh_release")

    all_cases = cases()
    failures = []
    implementations = {}
    variants = (
        ("default", []),
        ("to_chars", ["-DREADER_SYNC_USE_TO_CHARS=1"]),
        ("printf", ["-DREADER_SYNC_USE_TO_CHARS=0"]),
    )
    with tempfile.TemporaryDirectory(prefix="reader-canonical-") as scratch:
        for label, extra in variants:
            binary = Path(scratch) / f"canonical_smoke_{label}"
            command = compile_smoke(binary, extra)
            impl, results = build(binary, all_cases)
            implementations[label] = results
            if label == "printf":
                print("compile:", " ".join(command))
            print(f"[{label}] float formatter: {impl}")

            # Verify every reported line, including the fixtures the C++
            # harness wrote with ArduinoJson's own writer.
            for name, got in results.items():
                raw = base64.b64decode(got["raw_b64"])
                want_ok, want_record, want_uuid = expected(raw, got["device"], int(got["now"]))
                if bool(got["ok"]) != want_ok:
                    failures.append(
                        f"[{label}] {name}: ok={got['ok']} expected {want_ok} "
                        f"(error={got['error']!r}) raw={raw[:120]!r}"
                    )
                    continue
                if not want_ok:
                    if not got["error"]:
                        failures.append(f"[{label}] {name}: refused without an error message")
                    if got["record"] or got["uuid"]:
                        failures.append(f"[{label}] {name}: refused but returned a partial record")
                    continue
                if got["record"].encode("utf-8") != want_record:
                    failures.append(
                        f"[{label}] {name}: record mismatch\n  c++: {got['record'][:600]}\n  py : {want_record.decode('utf-8')[:600]}"
                    )
                if got["uuid"] != want_uuid:
                    failures.append(f"[{label}] {name}: uuid {got['uuid']} != {want_uuid}")
            for name, got in results.items():
                # Everything the C++ harness wrote with ArduinoJson's own writer
                # must also parse with ArduinoJson.
                if name.startswith("aj/") and not got["aj_accepts"]:
                    failures.append(f"[{label}] {name}: ArduinoJson could not parse its own writer output")
            for item in all_cases:
                if item["name"] not in results:
                    failures.append(f"[{label}] {item['name']}: no result line")

        reference = implementations["default"]
        for label in ("to_chars", "printf"):
            for name, entry in reference.items():
                other = implementations[label].get(name)
                if other != entry:
                    failures.append(f"{name}: {label} path disagrees with the default build: {entry} vs {other}")

    total = len(all_cases)
    print(f"cases: {total}  verified entries per build: {len(implementations['default'])}  "
          f"accepted: {sum(1 for i in all_cases if expected(i['raw'], i['device'], i['now'])[0])}")
    if failures:
        print(f"FAIL ({len(failures)})")
        for failure in failures:
            print(" -", failure)
        return 1
    print("PASS: uploadRecord and UUID match the companion byte for byte")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
