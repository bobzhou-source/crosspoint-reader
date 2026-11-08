"""The book concept index (`inbox-concepts.idx`) the reader's ConceptEngine searches.

Each EPUB in the card's /books/Inbox and /books/Books gets one 192-byte ICPT
record whose vector is computed here exactly as the firmware ConceptEngine
computes it: the int8 Platonic H-Net in `platonic-hnet.bin` (plus the optional
`engram.bin` table) when the model exists, otherwise the model-free byte-trigram
hash encoder. Inputs are capped at 256 bytes. The integer forward runs on numpy
when it is installed (milliseconds per book) and in pure Python otherwise
(about a second per book); both give identical bytes.
"""
from __future__ import annotations

import hashlib
import html
import json
import math
import re
import struct
import zipfile
from operator import mul
from pathlib import Path, PurePosixPath

try:
    import numpy as _np
except ImportError:  # optional: the pure-Python path is the reference
    _np = None

from .common import ReaderError, atomic_write, bounded_read

MODEL_NAME = "platonic-hnet.bin"
ENGRAM_NAME = "engram.bin"
INDEX_NAME = "inbox-concepts.idx"
# What the last card sync wrote: per-book stamps (so an unchanged book keeps its vector) and the
# SHA-256 of the index it produced (so records another exporter wrote are never claimed or dropped).
STATE_NAME = ".inbox-concepts.json"
STATE_VERSION = 1
INDEXED_SHELVES = ("books/Inbox", "books/Books")
FALLBACK_ENCODER = "fallback-trigram-v1"

MODEL_MAGIC = 0x504C484E  # "PLHN"
MODEL_HEADER = struct.Struct("<IHHHBBBBffffH")
D_BYTE = 64
D_CONCEPT = 128
ROUTER_DIM = 32
CORE_LOOPS = 2
ACT_SHIFT = 5  # activations are int8 at scale 2^-5
MAX_CHUNK_BYTES = 12
MAX_MODEL_BYTES = 2_400_000

ENGRAM_MAGIC = 0x454E4752  # "ENGR"
ENGRAM_HEADER = struct.Struct("<IIIIf")
ENGRAM_HEADER_BYTES = 512
ENGRAM_SLOT_BYTES = 64
ENGRAM_DELTA_DIMS = 24
MAX_ENGRAM_BYTES = 64 * 1024 * 1024

INDEX_MAGIC = 0x49435054  # "ICPT"
INDEX_VERSION = 1
INDEX_HEADER = struct.Struct("<IIII")
INDEX_RECORD = struct.Struct("<QII48s128b")
LABEL_BYTES = 48
MAX_INPUT_BYTES = 256
MAX_EPUB_MEMBER_BYTES = 4 * 1024 * 1024

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
GOLDEN = 0x9E3779B97F4A7C15
MASK64 = (1 << 64) - 1

# (name, rows, columns, scale field) in file order; every matrix is [in][out].
MODEL_TENSORS = (
    ("byte_embed", 256, D_BYTE, "embed"),
    ("conv1_dw", D_BYTE, 3, "proj"), ("conv1_pw", D_BYTE, D_BYTE, "proj"), ("conv1_gate", D_BYTE, D_BYTE, "proj"),
    ("conv2_dw", D_BYTE, 3, "proj"), ("conv2_pw", D_BYTE, D_BYTE, "proj"), ("conv2_gate", D_BYTE, D_BYTE, "proj"),
    ("router_q", D_BYTE, ROUTER_DIM, "router"), ("router_k", D_BYTE, ROUTER_DIM, "router"),
    ("chunk_proj", D_BYTE, D_CONCEPT, "proj"),
    ("core_conv_dw", D_CONCEPT, 3, "core"), ("core_conv_pw", D_CONCEPT, D_CONCEPT, "core"),
    ("core_gate", D_CONCEPT, D_CONCEPT, "core"),
    ("core_out_proj", D_CONCEPT, D_CONCEPT, "core"),
)
MODEL_PAYLOAD_BYTES = sum(rows * columns for _, rows, columns, _ in MODEL_TENSORS)


def fnv1a64(data: bytes) -> int:
    value = FNV_OFFSET
    for byte in data:
        value = ((value ^ byte) * FNV_PRIME) & MASK64
    return value


def _shift(value: int, amount: int) -> int:
    """sh(): round-half-up arithmetic right shift for amount > 0, left shift for amount < 0."""
    if amount > 0:
        return (value + (1 << (amount - 1))) >> amount
    return value << -amount if amount < 0 else value


def _sat8(value: int) -> int:
    return -127 if value < -127 else 127 if value > 127 else value


def _requantize(accumulator: int, amount: int) -> int:
    return _sat8(_shift(accumulator, amount))


def _exponent(scale: float, label: str) -> int:
    """S such that scale == 2^-S; every exported weight scale is an exact power of two."""
    if not math.isfinite(scale) or scale <= 0:
        raise ReaderError(f"{label} must be a positive power of two, not {scale!r}")
    return round(-math.log2(scale))


def _matvec(vector: list[int], columns: list[tuple[int, ...]], amount: int) -> list[int]:
    return [_requantize(sum(map(mul, vector, column)), amount) for column in columns]


def _round_divide(value: int, divisor: int) -> int:
    return (value + divisor // 2) // divisor if value >= 0 else -((-value + divisor // 2) // divisor)


def normalize127(vector: list[int]) -> list[int]:
    """Integer L2 normalization at scale 127: exact isqrt, rounded division, clamped to ±127."""
    norm = math.isqrt(sum(value * value for value in vector))
    if norm == 0:
        return [0] * len(vector)
    return [max(-127, min(127, _round_divide(127 * value, norm))) for value in vector]


def _columns(rows: list[tuple[int, ...]]) -> list[tuple[int, ...]]:
    return list(zip(*rows))


def _np_requantize(accumulator, amount: int):
    if amount > 0:
        accumulator = (accumulator + (1 << (amount - 1))) >> amount
    elif amount < 0:
        accumulator = accumulator << -amount
    return _np.clip(accumulator, -127, 127)


class _Block:
    """LFM2 double-gated ShortConv with a hard sigmoid, integer only (ACT=5 int8 activations)."""

    def __init__(self, dw, pw, gate, amount: int, out_proj=None):
        self.dw = dw
        self.pw = _columns(pw)
        self.gate = _columns(gate)
        self.amount = amount
        self.out_proj = _columns(out_proj) if out_proj is not None else None
        if _np is not None:
            self.arrays = tuple(_np.array(matrix, dtype=_np.int64) if matrix is not None else None
                                for matrix in (dw, pw, gate, out_proj))

    def __call__(self, sequence: list[list[int]]) -> list[list[int]]:
        zero = [0] * len(self.dw)
        gated, gates = [], []
        for current in sequence:
            gate = [min(128, max(0, value + 64)) for value in _matvec(current, self.gate, self.amount)]
            gates.append(gate)
            gated.append([(value * scale + 64) >> 7 for value, scale in zip(current, gate)])
        output = []
        for index, current in enumerate(sequence):
            back2 = gated[index - 2] if index >= 2 else zero
            back1 = gated[index - 1] if index >= 1 else zero
            now = gated[index]
            mixed = [_requantize(taps[0] * back2[channel] + taps[1] * back1[channel] + taps[2] * now[channel],
                                 self.amount)
                     for channel, taps in enumerate(self.dw)]
            update = [(value * scale + 64) >> 7
                      for value, scale in zip(_matvec(mixed, self.pw, self.amount), gates[index])]
            if self.out_proj is not None:
                update = _matvec(update, self.out_proj, self.amount)
            output.append([_sat8(value + add) for value, add in zip(current, update)])
        return output

    def numpy(self, sequence):
        """The same block over a (T, C) int64 array, all time steps at once."""
        dw, pw, gate, out_proj = self.arrays
        gates = _np.clip(_np_requantize(sequence @ gate, self.amount) + 64, 0, 128)
        gated = (sequence * gates + 64) >> 7
        padded = _np.vstack([_np.zeros((2, sequence.shape[1]), dtype=_np.int64), gated])
        mixed = _np_requantize(dw[:, 0] * padded[:-2] + dw[:, 1] * padded[1:-1] + dw[:, 2] * padded[2:], self.amount)
        update = (_np_requantize(mixed @ pw, self.amount) * gates + 64) >> 7
        if out_proj is not None:
            update = _np_requantize(update @ out_proj, self.amount)
        return _np.clip(sequence + update, -127, 127)


class Engram:
    """The sector-aligned span table; only the slots a text touches are read."""

    def __init__(self, data: bytes):
        if len(data) < ENGRAM_HEADER_BYTES:
            raise ReaderError("engram.bin is shorter than its 512-byte header")
        magic, version, slots, slot_bytes, scale = ENGRAM_HEADER.unpack_from(data)
        if magic != ENGRAM_MAGIC or version != 1 or slot_bytes != ENGRAM_SLOT_BYTES or slots < 1:
            raise ReaderError("engram.bin is not a version-1 ENGR table with 64-byte slots")
        if len(data) < ENGRAM_HEADER_BYTES + slots * ENGRAM_SLOT_BYTES:
            raise ReaderError(f"engram.bin is truncated: {slots} slots need "
                              f"{ENGRAM_HEADER_BYTES + slots * ENGRAM_SLOT_BYTES} bytes")
        self.data = data
        self.slots = slots
        self.amount = _exponent(scale, "engram.bin scale") - ACT_SHIFT

    def delta(self, span: bytes) -> list[int]:
        """The slot's delta at activation scale; always applied, with empty slots all zero."""
        offset = ENGRAM_HEADER_BYTES + (fnv1a64(span) % self.slots) * ENGRAM_SLOT_BYTES + 8
        return [_requantize(value, self.amount)
                for value in struct.unpack_from(f"<{ENGRAM_DELTA_DIMS}b", self.data, offset)]


def _utf8_length(byte: int) -> int:
    return 1 if byte < 0x80 else 4 if byte >= 0xF0 else 3 if byte >= 0xE0 else 2 if byte >= 0xC0 else 1


class PlatonicHNet:
    """The PLHN v1 integer reference forward, bit-exact with the firmware ConceptEngine."""

    def __init__(self, data: bytes, engram: Engram | None = None):
        if len(data) > MAX_MODEL_BYTES:
            raise ReaderError(f"platonic-hnet.bin is larger than {MAX_MODEL_BYTES} bytes")
        if len(data) < MODEL_HEADER.size + MODEL_PAYLOAD_BYTES:
            raise ReaderError(f"platonic-hnet.bin is truncated: needs {MODEL_HEADER.size + MODEL_PAYLOAD_BYTES} bytes")
        (magic, version, d_byte, d_concept, n_prelude, n_core, router_dim, _buckets_log2,
         scale_embed, scale_router, scale_proj, scale_core, _reserved) = MODEL_HEADER.unpack_from(data)
        if magic != MODEL_MAGIC or version != 1:
            raise ReaderError("platonic-hnet.bin is not a version-1 PLHN model")
        if (d_byte, d_concept, n_prelude, n_core, router_dim) != (D_BYTE, D_CONCEPT, 2, 2, ROUTER_DIM):
            raise ReaderError(f"platonic-hnet.bin has unsupported dimensions "
                              f"d_byte={d_byte} d_concept={d_concept} n_prelude={n_prelude} "
                              f"n_core={n_core} router_dim={router_dim}")
        exponents = {name: _exponent(value, f"platonic-hnet.bin scale_{name}") for name, value in
                     (("embed", scale_embed), ("router", scale_router), ("proj", scale_proj), ("core", scale_core))}
        tensors = {}
        offset = MODEL_HEADER.size
        for name, rows, columns, _ in MODEL_TENSORS:
            values = struct.unpack_from(f"<{rows * columns}b", data, offset)
            offset += rows * columns
            tensors[name] = [values[row * columns:(row + 1) * columns] for row in range(rows)]
        embed_shift = exponents["embed"] - ACT_SHIFT
        self.embed = [[_requantize(value, embed_shift) for value in row] for row in tensors["byte_embed"]]
        self.prelude = [_Block(tensors[f"{name}_dw"], tensors[f"{name}_pw"], tensors[f"{name}_gate"],
                               exponents["proj"]) for name in ("conv1", "conv2")]
        self.router_shift = exponents["router"]
        self.router_q = _columns(tensors["router_q"])
        self.router_k = _columns(tensors["router_k"])
        self.proj_shift = exponents["proj"]
        self.chunk_proj = _columns(tensors["chunk_proj"])
        self.core = _Block(tensors["core_conv_dw"], tensors["core_conv_pw"], tensors["core_gate"],
                           exponents["core"], out_proj=tensors["core_out_proj"])
        self.engram = engram
        if _np is not None:
            self.arrays = {name: _np.array(tensors[name], dtype=_np.int64)
                           for name in ("router_q", "router_k", "chunk_proj")}
            self.arrays["byte_embed"] = _np.array(self.embed, dtype=_np.int64)

    @staticmethod
    def _spans(text: bytes, dots: list[int]) -> list[tuple[int, int]]:
        """[start, end) chunks: router dot q_t·k_{t-1} <= 0, never splitting a UTF-8 character, at most 12 bytes."""
        starts = []
        length = 0
        for index, byte in enumerate(text):
            if (index == 0 or length >= MAX_CHUNK_BYTES
                    or ((byte & 0xC0) != 0x80 and (length + _utf8_length(byte) > MAX_CHUNK_BYTES
                                                   or dots[index] <= 0))):
                starts.append(index)
                length = 0
            length += 1
        return list(zip(starts, starts[1:] + [len(text)]))

    def chunks(self, text: bytes) -> list[tuple[int, int]]:
        text = text[:MAX_INPUT_BYTES]
        return self._forward(text)[0] if text else []

    def encode(self, text: bytes) -> list[int]:
        text = text[:MAX_INPUT_BYTES]
        if not text:
            return [0] * D_CONCEPT
        return self._forward(text)[1]

    def _forward(self, text: bytes) -> tuple[list[tuple[int, int]], list[int]]:
        return self._forward_numpy(text) if _np is not None else self._forward_python(text)

    def _deltas(self, text: bytes, spans: list[tuple[int, int]]) -> list[list[int]] | None:
        return [self.engram.delta(text[start:end]) for start, end in spans] if self.engram is not None else None

    def _forward_python(self, text: bytes) -> tuple[list[tuple[int, int]], list[int]]:
        states = [self.embed[byte] for byte in text]
        for block in self.prelude:
            states = block(states)
        queries = [_matvec(state, self.router_q, self.router_shift) for state in states]
        keys = [_matvec(state, self.router_k, self.router_shift) for state in states]
        dots = [0] + [sum(map(mul, queries[index], keys[index - 1])) for index in range(1, len(states))]
        spans = self._spans(text, dots)
        deltas = self._deltas(text, spans)
        concepts = []
        for number, (start, end) in enumerate(spans):
            reciprocal = (65536 + (end - start) // 2) // (end - start)
            pooled = [_sat8((sum(column) * reciprocal + 32768) >> 16) for column in zip(*states[start:end])]
            concept = _matvec(pooled, self.chunk_proj, self.proj_shift)
            if deltas is not None:
                concept[:ENGRAM_DELTA_DIMS] = [_sat8(value + add) for value, add in zip(concept, deltas[number])]
            concepts.append(concept)
        for _ in range(CORE_LOOPS):
            concepts = self.core(concepts)
        return spans, normalize127([sum(column) for column in zip(*concepts)])

    def _forward_numpy(self, text: bytes) -> tuple[list[tuple[int, int]], list[int]]:
        arrays = self.arrays
        states = arrays["byte_embed"][_np.frombuffer(text, dtype=_np.uint8)]
        for block in self.prelude:
            states = block.numpy(states)
        queries = _np_requantize(states @ arrays["router_q"], self.router_shift)
        keys = _np_requantize(states @ arrays["router_k"], self.router_shift)
        dots = [0] + (queries[1:] * keys[:-1]).sum(axis=1).tolist()
        spans = self._spans(text, dots)
        pooled = _np.empty((len(spans), D_BYTE), dtype=_np.int64)
        for number, (start, end) in enumerate(spans):
            reciprocal = (65536 + (end - start) // 2) // (end - start)
            pooled[number] = (states[start:end].sum(axis=0) * reciprocal + 32768) >> 16
        concepts = _np_requantize(_np.clip(pooled, -127, 127) @ arrays["chunk_proj"], self.proj_shift)
        deltas = self._deltas(text, spans)
        if deltas is not None:
            concepts[:, :ENGRAM_DELTA_DIMS] = _np.clip(
                concepts[:, :ENGRAM_DELTA_DIMS] + _np.array(deltas, dtype=_np.int64), -127, 127)
        for _ in range(CORE_LOOPS):
            concepts = self.core.numpy(concepts)
        return spans, normalize127(concepts.sum(axis=0).tolist())


def fallback_encode(text: bytes) -> list[int]:
    """The model-free encoder: signed bits of hashed byte trigrams, ASCII case folded."""
    text = text[:MAX_INPUT_BYTES].translate(_ASCII_LOWER)
    if not text:
        return [0] * D_CONCEPT
    grams = [text] if len(text) < 3 else [text[index:index + 3] for index in range(len(text) - 2)]
    accumulator = [0] * D_CONCEPT
    for gram in grams:
        first = fnv1a64(gram)
        second = ((first * GOLDEN) & MASK64) ^ (first >> 29)
        for dimension in range(D_CONCEPT):
            bits = first if dimension < 64 else second
            accumulator[dimension] += 1 if (bits >> (dimension % 64)) & 1 else -1
    return normalize127(accumulator)


_ASCII_LOWER = bytes.maketrans(bytes(range(65, 91)), bytes(range(97, 123)))


def _utf8_prefix(text: str, limit: int) -> bytes:
    data = text.encode("utf-8")[:limit]
    return data.decode("utf-8", "ignore").encode("utf-8")


_TAG = re.compile(r"<[^>]*>")
_HIDDEN = re.compile(r"<(script|style|head)\b.*?</\1\s*>", re.IGNORECASE | re.DOTALL)
_SPACE = re.compile(r"\s+")


def _xml_text(markup: str) -> str:
    return _SPACE.sub(" ", html.unescape(_TAG.sub(" ", _HIDDEN.sub(" ", markup)))).strip()


def epub_text(path: Path, limit: int = MAX_INPUT_BYTES) -> tuple[str, bytes]:
    """(title, encoder input): dc:title (else the file stem) and title + newline + spine text, cut to `limit` bytes."""
    title, body = "", []
    try:
        with zipfile.ZipFile(path) as archive:
            def member(name: str) -> str:
                info = archive.getinfo(name)
                if info.file_size > MAX_EPUB_MEMBER_BYTES:
                    raise ReaderError(f"{name} is too large")
                return archive.read(info).decode("utf-8", "replace")

            container = member("META-INF/container.xml")
            found = re.search(r"full-path\s*=\s*[\"']([^\"']+)", container)
            if found is None:
                raise ReaderError("no rootfile")
            opf_name = html.unescape(found.group(1))
            opf = member(opf_name)
            found = re.search(r"<dc:title\b[^>]*>(.*?)</dc:title>", opf, re.IGNORECASE | re.DOTALL)
            title = _xml_text(found.group(1)) if found else ""
            hrefs = {}
            for item in re.finditer(r"<item\b[^>]*>", opf, re.IGNORECASE):
                attributes = dict(re.findall(r"([\w:-]+)\s*=\s*[\"']([^\"']*)[\"']", item.group(0)))
                if "id" in attributes and "href" in attributes:
                    hrefs[attributes["id"]] = html.unescape(attributes["href"])
            base = PurePosixPath(opf_name).parent
            size = len(title.encode("utf-8")) + 1
            for reference in re.finditer(r"<itemref\b[^>]*\bidref\s*=\s*[\"']([^\"']+)", opf, re.IGNORECASE):
                href = hrefs.get(reference.group(1))
                if href is None:
                    continue
                name = (base / href.split("#", 1)[0]).as_posix()
                try:
                    text = _xml_text(member(name))
                except (KeyError, ReaderError):
                    continue
                if text:
                    body.append(text)
                    size += len(text.encode("utf-8")) + 1
                if size >= limit:
                    break
    except (OSError, zipfile.BadZipFile, KeyError, ReaderError, RuntimeError, ValueError):
        pass
    title = title or path.stem
    return title, (title + "\n" + " ".join(body)).encode("utf-8")[:limit]


def _label(title: str) -> bytes:
    return _utf8_prefix(title, LABEL_BYTES - 1)


def pack_index(records: list[dict]) -> bytes:
    """ICPT file: 16-byte header + one 192-byte record each; records read from a file keep their exact bytes."""
    parts = [INDEX_HEADER.pack(INDEX_MAGIC, INDEX_VERSION, len(records), D_CONCEPT)]
    for record in records:
        parts.append(record.get("raw") or INDEX_RECORD.pack(
            record["path_hash"], record["page_or_chunk"], record["timestamp_or_flags"] & 0xFFFFFFFF,
            _label(record["label"]), *record["vec"]))
    return b"".join(parts)


def read_index(data: bytes) -> list[dict]:
    if len(data) < INDEX_HEADER.size:
        raise ReaderError("inbox-concepts.idx is shorter than its 16-byte header")
    magic, version, count, dim = INDEX_HEADER.unpack_from(data)
    if magic != INDEX_MAGIC or version != INDEX_VERSION or dim != D_CONCEPT:
        raise ReaderError("inbox-concepts.idx is not a version-1 ICPT index of 128-d vectors")
    if len(data) != INDEX_HEADER.size + count * INDEX_RECORD.size:
        raise ReaderError(f"inbox-concepts.idx holds {len(data)} bytes, not {count} records")
    records = []
    for index in range(count):
        offset = INDEX_HEADER.size + index * INDEX_RECORD.size
        path_hash, page, stamp, label, *vec = INDEX_RECORD.unpack_from(data, offset)
        records.append({"path_hash": path_hash, "page_or_chunk": page, "timestamp_or_flags": stamp,
                        "label": label.split(b"\0", 1)[0].decode("utf-8", "replace"), "vec": vec,
                        "raw": data[offset:offset + INDEX_RECORD.size]})
    return records


def card_books(card_root: Path) -> list[tuple[str, Path]]:
    """("/books/Inbox/x.epub", path) for every visible EPUB under the indexed card shelves, sorted."""
    books = []
    for shelf in INDEXED_SHELVES:
        root = card_root / shelf
        if root.is_symlink() or not root.is_dir():
            continue
        pending = [root]
        while pending:
            folder = pending.pop()
            for entry in folder.iterdir():
                if entry.name.startswith(".") or entry.is_symlink():
                    continue
                if entry.is_dir():
                    pending.append(entry)
                elif entry.is_file() and entry.suffix.lower() == ".epub":
                    books.append(("/" + entry.relative_to(card_root).as_posix(), entry))
    return sorted(books)


def _encoder_identity(models_dir: Path) -> tuple[str, PlatonicHNet | None]:
    model_path = models_dir / MODEL_NAME
    if not model_path.is_file() or model_path.is_symlink():
        return FALLBACK_ENCODER, None
    model_data = bounded_read(model_path, MAX_MODEL_BYTES)
    identity = f"{MODEL_NAME}:{hashlib.sha256(model_data).hexdigest()}"
    engram = None
    engram_path = models_dir / ENGRAM_NAME
    if engram_path.is_file() and not engram_path.is_symlink():
        engram_data = bounded_read(engram_path, MAX_ENGRAM_BYTES)
        engram = Engram(engram_data)
        identity += f"+{ENGRAM_NAME}:{hashlib.sha256(engram_data).hexdigest()}"
    return identity, PlatonicHNet(model_data, engram)


def _load_state(models_dir: Path) -> dict:
    try:
        state = json.loads((models_dir / STATE_NAME).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    if not isinstance(state, dict) or state.get("version") != STATE_VERSION or not isinstance(state.get("books"), dict):
        return {}
    return state


def refresh_index(models_dir: Path, card_root: Path) -> dict:
    """Bring `models_dir/inbox-concepts.idx` up to date with the card's Inbox and Books EPUBs.

    Records this function did not write (the trainer's exporter covers /books/Books and /books/Novels
    passages) are kept verbatim, and a book that already has one gets no second record. Each of the
    remaining books keeps its previous vector while its size, mtime and the encoder (model and
    engram hashes) are unchanged. The file is rewritten only when its bytes would differ.
    """
    models_dir = Path(models_dir)
    encoder, model = _encoder_identity(models_dir)
    index_path = models_dir / INDEX_NAME
    state = _load_state(models_dir)
    current = None
    existing: list[dict] = []
    if index_path.is_file() and not index_path.is_symlink():
        current = index_path.read_bytes()
        try:
            existing = read_index(current)
        except ReaderError:
            existing = []
    ours = current is not None and state.get("index_sha256") == hashlib.sha256(current).hexdigest()
    owned = {fnv1a64(path.encode("utf-8")) for path in state.get("books", {})} if ours else set()
    foreign = [record for record in existing if record["path_hash"] not in owned]
    covered = {record["path_hash"] for record in foreign}
    previous = ({record["path_hash"]: record for record in existing if record["path_hash"] in owned}
                if state.get("encoder") == encoder else {})
    stamps = state.get("books", {})
    records, new_stamps, encoded = [], {}, 0
    for card_path, path in card_books(card_root):
        path_hash = fnv1a64(card_path.encode("utf-8"))
        if path_hash in covered:
            continue
        status = path.stat()
        stamp = [status.st_size, status.st_mtime_ns]
        record = previous.get(path_hash)
        if record is None or stamps.get(card_path) != stamp:
            title, text = epub_text(path)
            vector = model.encode(text) if model is not None else fallback_encode(text)
            record = {"path_hash": path_hash, "page_or_chunk": 0,
                      "timestamp_or_flags": int(status.st_mtime) & 0xFFFFFFFF, "label": title, "vec": vector}
            encoded += 1
        records.append(record)
        new_stamps[card_path] = stamp
    data = pack_index(foreign + records)
    updated = current != data
    if updated:
        atomic_write(index_path, data)
    new_state = {"version": STATE_VERSION, "encoder": encoder, "books": new_stamps,
                 "index_sha256": hashlib.sha256(data).hexdigest()}
    if new_state != state:
        atomic_write(models_dir / STATE_NAME, json.dumps(new_state, indent=1, sort_keys=True).encode("utf-8"))
    return {"path": str(index_path), "encoder": encoder.split(":", 1)[0], "records": len(foreign) + len(records),
            "kept": len(foreign), "books": len(records), "encoded": encoded, "updated": updated}
