#!/usr/bin/env python3
"""Generate the private "Send to Reader" Apple Shortcut (macOS + iPhone).

The committed template (send-to-reader.template.wflow) is a plain, key-free
Shortcuts workflow. It takes a URL from the share sheet (or from the input that
`shortcuts run` supplies), POSTs `{"url": <url>}` to
https://robertzhou.dev/reader/links, and shows the returned link id + status.

This script injects the dedicated send-only bearer key *at generation time* from
the macOS Keychain, signs the result with Apple's own signer, and writes a
private (0600) .shortcut file for import on this Mac and on the iPhone.

The key is never written to this repo, never printed, and never passed on the
command line. Nothing is uploaded to a third-party shortcut builder; the only
network step is Apple's official `shortcuts sign`.

Usage:
    python3 generate_send_to_reader_shortcut.py [--out PATH] [--install]
"""
from __future__ import annotations

import argparse
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_TEMPLATE = os.path.join(HERE, "send-to-reader.template.wflow")
DEFAULT_OUT = "/Users/b/papyrix/Reader Workbench/Setup/Send to Reader.shortcut"
PLACEHOLDER = "__READER_SHARE_KEY__"
DEFAULT_SERVICE = "reader-workbench:share-key"
DEFAULT_ACCOUNT = "b"
KEY_RE = re.compile(r"^[0-9a-f]{64}$")


def read_key(service: str, account: str) -> str:
    """Read the send-only key from the login Keychain (never printed)."""
    proc = subprocess.run(
        ["security", "find-generic-password", "-s", service, "-a", account, "-w"],
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        sys.exit(
            f"error: no Keychain item {service!r} (account {account!r}). "
            "The backend owner must create the dedicated send-only key first; "
            "the main sync key must not be used."
        )
    key = proc.stdout.strip()
    if not KEY_RE.match(key):
        sys.exit("error: Keychain item is not a 64-character lowercase hex key; refusing to embed it")
    return key


def replace_placeholder(node, key: str, hits: list) -> None:
    if isinstance(node, dict):
        for k, v in list(node.items()):
            if isinstance(v, str) and PLACEHOLDER in v:
                hits.append(1)
                node[k] = v.replace(PLACEHOLDER, key)
            else:
                replace_placeholder(v, key, hits)
    elif isinstance(node, list):
        for i, v in enumerate(node):
            if isinstance(v, str) and PLACEHOLDER in v:
                hits.append(1)
                node[i] = v.replace(PLACEHOLDER, key)
            else:
                replace_placeholder(v, key, hits)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--template", default=DEFAULT_TEMPLATE)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--service", default=DEFAULT_SERVICE)
    ap.add_argument("--account", default=DEFAULT_ACCOUNT)
    ap.add_argument("--mode", default="anyone", choices=("anyone", "people-who-know-me"))
    ap.add_argument("--no-sign", action="store_true", help="keep the unsigned .wflow next to the output")
    ap.add_argument("--install", action="store_true", help="open the signed shortcut in Shortcuts for review")
    args = ap.parse_args()

    if not os.path.exists(args.template):
        sys.exit(f"error: template not found: {args.template}")
    key = read_key(args.service, args.account)

    with open(args.template, "rb") as fh:
        workflow = plistlib.load(fh)
    hits: list = []
    replace_placeholder(workflow, key, hits)
    if len(hits) != 1:
        sys.exit(f"error: expected exactly one {PLACEHOLDER} slot in the template, found {len(hits)}")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    tmpdir = tempfile.mkdtemp(prefix="s2r-")
    unsigned = os.path.join(tmpdir, "Send to Reader.wflow")
    with open(unsigned, "wb") as fh:
        plistlib.dump(workflow, fh, fmt=plistlib.FMT_BINARY)
    os.chmod(unsigned, 0o600)

    if args.no_sign:
        shutil.copyfile(unsigned, args.out + ".wflow")
        os.chmod(args.out + ".wflow", 0o600)
        shutil.rmtree(tmpdir, ignore_errors=True)
        print(f"wrote unsigned workflow: {args.out}.wflow")
        return

    proc = subprocess.run(
        ["shortcuts", "sign", "-m", args.mode, "-i", unsigned, "-o", args.out],
        capture_output=True,
        text=True,
    )
    shutil.rmtree(tmpdir, ignore_errors=True)
    if proc.returncode != 0:
        sys.exit(f"error: shortcuts sign failed: {proc.stderr.strip() or proc.stdout.strip()}")
    os.chmod(args.out, 0o600)
    print(f"signed shortcut: {args.out} ({os.path.getsize(args.out)} bytes, mode 0600)")
    print("import on this Mac: open the file, review the actions, click Add Shortcut")
    print("import on iPhone: transfer the file to the phone, tap it, Add Shortcut")

    if args.install:
        subprocess.run(["open", args.out], check=False)


if __name__ == "__main__":
    main()
