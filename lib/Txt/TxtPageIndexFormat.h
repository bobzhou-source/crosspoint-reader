#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// On-disk layout of a TXT page index (`<cache>/index.bin`), format version 4. Every integer is little-endian:
//
//    0  u32  magic "TXTI" (0x54585449)
//    4  u8   version (4)
//    5  u8   state: 1 = complete, 0 = partial (resumable)
//    6  u16  reserved (0)
//    8  u32  source file size
//   12  u32  source signature (modification time and head/tail bytes)
//   16  u32  layout revision
//   20  i32  viewport width
//   24  i32  lines per page
//   28  i32  reader font id
//   32  i32  screen margin
//   36  u8   paragraph alignment
//   37  u8   layout flags (LAYOUT_CJK_PARAGRAPHS)
//   38  i16  first-line indent in pixels
//   40  u32  page count N
//   44  u32  chapter count C
//   48  u32  FNV-1a checksum of the N offsets and C chapter records as stored
//   52  N x u32  page start offsets
//       C x (u32 heading offset, u32 page)
//
// A complete index lists every page. A partial one lists the pages found so
// far; its last offset is the next page to lay out, and its chapters are the
// headings on the pages before it. Both are replaced only through a temporary
// file, so a reader always finds the previous valid index, a resumable partial,
// or nothing.
namespace TxtPageIndexFormat {

inline constexpr uint32_t FORMAT_MAGIC = 0x54585449;
inline constexpr uint8_t FORMAT_VERSION = 4;
inline constexpr size_t HEADER_BYTES = 52;
// Bumped whenever line breaking or paragraph style moves page starts, so every book is paginated once more.
inline constexpr uint32_t LAYOUT_REVISION = 2;
inline constexpr uint8_t LAYOUT_CJK_PARAGRAPHS = 0x01;

// The version 3 index written by earlier firmware: magic, u8 version 3, u32 file size, five layout fields, u32
// page count N at 26, then N x u32 offsets from byte 30. Read only to carry a saved page over to a byte offset.
inline constexpr uint8_t LEGACY_VERSION = 3;
inline constexpr size_t LEGACY_HEADER_BYTES = 30;

// Everything besides the offsets that a stored index must agree with before its offsets are reused.
struct Key {
  uint32_t fileSize;
  uint32_t sourceSignature;
  int32_t viewportWidth;
  int32_t linesPerPage;
  int32_t fontId;
  int32_t screenMargin;
  uint8_t paragraphAlignment;
  uint8_t layoutFlags;
  int16_t firstLineIndent;
};

enum class Check : uint8_t {
  Ok,
  BadMagic,
  BadVersion,
  BadState,
  LayoutRevisionChanged,
  FileSizeChanged,
  SourceChanged,
  LayoutChanged,
  BadPageCount,
  BadLength,
  BadChecksum,
  BadOffsets,
  BadChapters,
  ReadFailed,
};

inline const char* describe(const Check check) {
  switch (check) {
    case Check::Ok:
      return "ok";
    case Check::BadMagic:
      return "magic mismatch";
    case Check::BadVersion:
      return "version mismatch";
    case Check::BadState:
      return "unknown state";
    case Check::LayoutRevisionChanged:
      return "layout revision changed";
    case Check::FileSizeChanged:
      return "file size changed";
    case Check::SourceChanged:
      return "source changed";
    case Check::LayoutChanged:
      return "layout changed";
    case Check::BadPageCount:
      return "no pages";
    case Check::BadLength:
      return "length does not match the page and chapter counts";
    case Check::BadChecksum:
      return "checksum mismatch";
    case Check::BadOffsets:
      return "offsets are not a valid page list";
    case Check::BadChapters:
      return "chapters are not a valid heading list";
    case Check::ReadFailed:
      return "read failed";
  }
  return "unknown";
}

inline uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

inline void writeLe32(uint8_t* p, const uint32_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
  p[2] = static_cast<uint8_t>(value >> 16);
  p[3] = static_cast<uint8_t>(value >> 24);
}

inline constexpr uint32_t FNV_OFFSET = 2166136261u;

inline uint32_t fnv1a(uint32_t hash, const uint8_t* data, const size_t length) {
  for (size_t i = 0; i < length; i++) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

inline uint32_t fnv1aLe32(const uint32_t hash, const uint32_t value) {
  uint8_t bytes[4];
  writeLe32(bytes, value);
  return fnv1a(hash, bytes, 4);
}

// Index of the page containing `offset` (offsets[0] == 0, strictly increasing).
inline uint32_t pageOf(const std::vector<uint32_t>& offsets, const uint32_t offset) {
  size_t lo = 0;
  size_t hi = offsets.size();
  while (hi - lo > 1) {
    const size_t mid = lo + (hi - lo) / 2;
    if (offsets[mid] <= offset) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return static_cast<uint32_t>(lo);
}

// Checksum of the body exactly as written: offsets, then (heading offset, page) records.
inline uint32_t bodyChecksum(const std::vector<uint32_t>& offsets, const std::vector<uint32_t>& chapters) {
  uint32_t hash = FNV_OFFSET;
  for (const uint32_t offset : offsets) hash = fnv1aLe32(hash, offset);
  for (const uint32_t chapter : chapters) {
    hash = fnv1aLe32(hash, chapter);
    hash = fnv1aLe32(hash, pageOf(offsets, chapter));
  }
  return hash;
}

inline void encodeHeader(uint8_t* out, const Key& key, const bool complete, const uint32_t pageCount,
                         const uint32_t chapterCount, const uint32_t checksum) {
  writeLe32(out, FORMAT_MAGIC);
  out[4] = FORMAT_VERSION;
  out[5] = complete ? 1 : 0;
  out[6] = 0;
  out[7] = 0;
  writeLe32(out + 8, key.fileSize);
  writeLe32(out + 12, key.sourceSignature);
  writeLe32(out + 16, LAYOUT_REVISION);
  writeLe32(out + 20, static_cast<uint32_t>(key.viewportWidth));
  writeLe32(out + 24, static_cast<uint32_t>(key.linesPerPage));
  writeLe32(out + 28, static_cast<uint32_t>(key.fontId));
  writeLe32(out + 32, static_cast<uint32_t>(key.screenMargin));
  out[36] = key.paragraphAlignment;
  out[37] = key.layoutFlags;
  const auto indent = static_cast<uint16_t>(key.firstLineIndent);
  out[38] = static_cast<uint8_t>(indent);
  out[39] = static_cast<uint8_t>(indent >> 8);
  writeLe32(out + 40, pageCount);
  writeLe32(out + 44, chapterCount);
  writeLe32(out + 48, checksum);
}

struct Header {
  bool complete = false;
  uint32_t pageCount = 0;
  uint32_t chapterCount = 0;
  uint32_t checksum = 0;
};

// Validates the header of an index file of `totalBytes` bytes against the layout the reader is using now.
// On Ok the file length proves every offset and chapter record is present.
inline Check checkHeader(const uint8_t* header, const size_t totalBytes, const Key& expected, Header& out) {
  out = Header{};
  if (totalBytes < HEADER_BYTES) return Check::BadLength;
  if (readLe32(header) != FORMAT_MAGIC) return Check::BadMagic;
  if (header[4] != FORMAT_VERSION) return Check::BadVersion;
  if (header[5] > 1 || header[6] != 0 || header[7] != 0) return Check::BadState;
  if (readLe32(header + 16) != LAYOUT_REVISION) return Check::LayoutRevisionChanged;
  if (readLe32(header + 8) != expected.fileSize) return Check::FileSizeChanged;
  if (readLe32(header + 12) != expected.sourceSignature) return Check::SourceChanged;
  const auto indent = static_cast<int16_t>(static_cast<uint16_t>(header[38] | (header[39] << 8)));
  if (static_cast<int32_t>(readLe32(header + 20)) != expected.viewportWidth ||
      static_cast<int32_t>(readLe32(header + 24)) != expected.linesPerPage ||
      static_cast<int32_t>(readLe32(header + 28)) != expected.fontId ||
      static_cast<int32_t>(readLe32(header + 32)) != expected.screenMargin ||
      header[36] != expected.paragraphAlignment || header[37] != expected.layoutFlags ||
      indent != expected.firstLineIndent) {
    return Check::LayoutChanged;
  }

  const uint32_t pages = readLe32(header + 40);
  const uint32_t chapters = readLe32(header + 44);
  if (pages == 0) return Check::BadPageCount;
  if (static_cast<uint64_t>(totalBytes) != HEADER_BYTES + 4ULL * pages + 8ULL * chapters) return Check::BadLength;
  out.complete = header[5] == 1;
  out.pageCount = pages;
  out.chapterCount = chapters;
  out.checksum = readLe32(header + 48);
  return Check::Ok;
}

// A page list is valid when it starts at 0, strictly increases, and stays inside the source; an empty source has
// exactly the one page at 0.
inline Check checkOffsets(const std::vector<uint32_t>& offsets, const size_t fileSize) {
  if (offsets.empty() || offsets[0] != 0) return Check::BadOffsets;
  if (fileSize == 0) return offsets.size() == 1 ? Check::Ok : Check::BadOffsets;
  for (size_t i = 1; i < offsets.size(); i++) {
    if (offsets[i] <= offsets[i - 1] || offsets[i] >= fileSize) return Check::BadOffsets;
  }
  return Check::Ok;
}

// Headings strictly increase, lie inside the source and, in a partial index, on a page before the frontier.
inline Check checkChapters(const std::vector<uint32_t>& chapters, const std::vector<uint32_t>& offsets,
                           const size_t fileSize, const bool complete) {
  const size_t limit = complete ? fileSize : offsets.back();
  for (size_t i = 0; i < chapters.size(); i++) {
    if (chapters[i] >= limit || (i > 0 && chapters[i] <= chapters[i - 1])) return Check::BadChapters;
  }
  return Check::Ok;
}

// Streams an index through `write(const uint8_t*, size_t) -> bool` in small chunks.
template <typename Write>
bool writeIndex(Write&& write, const Key& key, const bool complete, const std::vector<uint32_t>& offsets,
                const std::vector<uint32_t>& chapters) {
  uint8_t header[HEADER_BYTES];
  encodeHeader(header, key, complete, static_cast<uint32_t>(offsets.size()), static_cast<uint32_t>(chapters.size()),
               bodyChecksum(offsets, chapters));
  if (!write(header, HEADER_BYTES)) return false;

  constexpr size_t CHUNK_WORDS = 32;
  uint8_t chunk[CHUNK_WORDS * 4];
  size_t used = 0;
  const auto put = [&](const uint32_t value) {
    writeLe32(chunk + used, value);
    used += 4;
    if (used < sizeof(chunk)) return true;
    used = 0;
    return write(chunk, sizeof(chunk));
  };
  for (const uint32_t offset : offsets) {
    if (!put(offset)) return false;
  }
  for (const uint32_t chapter : chapters) {
    if (!put(chapter) || !put(pageOf(offsets, chapter))) return false;
  }
  return used == 0 || write(chunk, used);
}

struct Loaded {
  bool complete = false;
  std::vector<uint32_t> offsets;
  std::vector<uint32_t> chapters;
};

// Reads and fully validates an index of `totalBytes` bytes through `read(uint8_t*, size_t) -> bool` (exact reads).
// On anything but Ok, `out` is left empty.
template <typename Read>
Check readIndex(Read&& read, const size_t totalBytes, const Key& expected, Loaded& out) {
  out = Loaded{};
  if (totalBytes < HEADER_BYTES) return Check::BadLength;
  uint8_t header[HEADER_BYTES];
  if (!read(header, HEADER_BYTES)) return Check::ReadFailed;
  Header parsed;
  const Check headerCheck = checkHeader(header, totalBytes, expected, parsed);
  if (headerCheck != Check::Ok) return headerCheck;

  Loaded loaded;
  loaded.complete = parsed.complete;
  loaded.offsets.reserve(parsed.pageCount);
  loaded.chapters.reserve(parsed.chapterCount);
  uint32_t hash = FNV_OFFSET;
  uint8_t word[4];
  for (uint32_t i = 0; i < parsed.pageCount; i++) {
    if (!read(word, 4)) return Check::ReadFailed;
    hash = fnv1a(hash, word, 4);
    loaded.offsets.push_back(readLe32(word));
  }
  const Check offsetsCheck = checkOffsets(loaded.offsets, expected.fileSize);
  if (offsetsCheck != Check::Ok) return offsetsCheck;
  for (uint32_t i = 0; i < parsed.chapterCount; i++) {
    uint8_t record[8];
    if (!read(record, 8)) return Check::ReadFailed;
    hash = fnv1a(hash, record, 8);
    const uint32_t chapter = readLe32(record);
    if (readLe32(record + 4) != pageOf(loaded.offsets, chapter)) return Check::BadChapters;
    loaded.chapters.push_back(chapter);
  }
  if (hash != parsed.checksum) return Check::BadChecksum;
  const Check chaptersCheck = checkChapters(loaded.chapters, loaded.offsets, expected.fileSize, loaded.complete);
  if (chaptersCheck != Check::Ok) return chaptersCheck;
  out = static_cast<Loaded&&>(loaded);
  return Check::Ok;
}

// Validates a version 3 header (see LEGACY_*) for a source of `fileSize` bytes; on success `pageCount` offsets
// follow at LEGACY_HEADER_BYTES.
inline bool checkLegacyHeader(const uint8_t* header, const size_t totalBytes, const uint32_t fileSize,
                              uint32_t& pageCount) {
  pageCount = 0;
  if (totalBytes < LEGACY_HEADER_BYTES) return false;
  if (readLe32(header) != FORMAT_MAGIC || header[4] != LEGACY_VERSION) return false;
  if (readLe32(header + 5) != fileSize) return false;
  const uint32_t pages = readLe32(header + 26);
  if (pages == 0 || static_cast<uint64_t>(totalBytes) != LEGACY_HEADER_BYTES + 4ULL * pages) return false;
  pageCount = pages;
  return true;
}

// Start offset of `page` (clamped to the last page) in a version 3 index of
// `totalBytes` bytes for a source of `fileSize` bytes, read through
// `readAt(size_t at, uint8_t* out, size_t length) -> bool` (exact reads).
// False when the file is not such an index or the offset lies outside the source.
template <typename ReadAt>
bool legacyPageOffset(ReadAt&& readAt, const size_t totalBytes, const uint32_t fileSize, const uint32_t page,
                      uint32_t& offset) {
  uint8_t header[LEGACY_HEADER_BYTES];
  uint32_t pageCount = 0;
  if (totalBytes < LEGACY_HEADER_BYTES || !readAt(0, header, LEGACY_HEADER_BYTES) ||
      !checkLegacyHeader(header, totalBytes, fileSize, pageCount)) {
    return false;
  }
  const uint32_t clamped = page < pageCount ? page : pageCount - 1;
  uint8_t word[4];
  if (!readAt(LEGACY_HEADER_BYTES + 4 * static_cast<size_t>(clamped), word, 4)) return false;
  const uint32_t found = readLe32(word);
  if (found != 0 && found >= fileSize) return false;
  offset = found;
  return true;
}

// Fingerprint of a layout, stored with the reading position.
inline uint32_t keyFingerprint(const Key& key) {
  uint8_t header[HEADER_BYTES];
  encodeHeader(header, key, false, 0, 0, 0);
  return fnv1a(FNV_OFFSET, header, HEADER_BYTES);
}

}  // namespace TxtPageIndexFormat

// Reading position (`<cache>/progress.bin`). Earlier firmware stored only the page number in the first four bytes
// (u16 page, two zero bytes); version 2 keeps them, so a downgrade still resumes, and appends the byte offset:
//
//    0  u16  page in the layout identified by the fingerprint
//    2  u16  0
//    4  u32  magic "TXP2" (0x32505854)
//    8  u32  source byte offset of the page start
//   12  u32  layout fingerprint (TxtPageIndexFormat::keyFingerprint)
//
// The byte offset is what positions the reader, so a book opened with a new layout still resumes at the same text.
namespace TxtProgressFormat {

inline constexpr uint32_t MAGIC = 0x32505854;
inline constexpr size_t BYTES = 16;
inline constexpr size_t LEGACY_BYTES = 4;

struct Position {
  enum class Kind : uint8_t { None, Page, Offset };
  Kind kind = Kind::None;
  uint32_t page = 0;
  uint32_t offset = 0;
  uint32_t fingerprint = 0;
};

inline void encode(uint8_t* out, const uint32_t page, const uint32_t offset, const uint32_t fingerprint) {
  const uint32_t storedPage = page > 0xFFFF ? 0xFFFF : page;
  out[0] = static_cast<uint8_t>(storedPage);
  out[1] = static_cast<uint8_t>(storedPage >> 8);
  out[2] = 0;
  out[3] = 0;
  TxtPageIndexFormat::writeLe32(out + 4, MAGIC);
  TxtPageIndexFormat::writeLe32(out + 8, offset);
  TxtPageIndexFormat::writeLe32(out + 12, fingerprint);
}

inline Position decode(const uint8_t* data, const size_t length) {
  Position position;
  if (length < LEGACY_BYTES) return position;
  position.page = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8);
  position.kind = Position::Kind::Page;
  if (length >= BYTES && TxtPageIndexFormat::readLe32(data + 4) == MAGIC) {
    position.kind = Position::Kind::Offset;
    position.offset = TxtPageIndexFormat::readLe32(data + 8);
    position.fingerprint = TxtPageIndexFormat::readLe32(data + 12);
  }
  return position;
}

}  // namespace TxtProgressFormat
