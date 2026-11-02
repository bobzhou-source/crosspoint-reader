#include <gtest/gtest.h>

#include <TxtPageIndexFormat.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace TxtPageIndexFormat;

namespace {

const Key kKey{165998, 0xC0FFEE11u, 440, 26, -1234567, 20, 2, LAYOUT_CJK_PARAGRAPHS, 56};
const std::vector<uint32_t> kOffsets{0, 1500, 3100, 165000};
const std::vector<uint32_t> kChapters{0, 1600, 165100};

// An index file exactly as writeIndex streams it.
std::vector<uint8_t> imageOf(const Key& key, const bool complete, const std::vector<uint32_t>& offsets,
                             const std::vector<uint32_t>& chapters) {
  std::vector<uint8_t> bytes;
  const bool written = writeIndex(
      [&bytes](const uint8_t* data, const size_t length) {
        bytes.insert(bytes.end(), data, data + length);
        return true;
      },
      key, complete, offsets, chapters);
  EXPECT_TRUE(written);
  return bytes;
}

// Reads `bytes` as an index file of `totalBytes` bytes; reads past `readable` bytes fail.
Check readImage(const std::vector<uint8_t>& bytes, const Key& key, Loaded& loaded,
                const size_t readable = std::numeric_limits<size_t>::max()) {
  size_t cursor = 0;
  const size_t limit = std::min(readable, bytes.size());
  return readIndex(
      [&](uint8_t* out, const size_t length) {
        if (cursor + length > limit) return false;
        std::memcpy(out, bytes.data() + cursor, length);
        cursor += length;
        return true;
      },
      bytes.size(), key, loaded);
}

Check readImage(const std::vector<uint8_t>& bytes, const Key& key) {
  Loaded loaded;
  const Check check = readImage(bytes, key, loaded);
  EXPECT_TRUE(check == Check::Ok || (loaded.offsets.empty() && loaded.chapters.empty()))
      << "a rejected index must leave nothing behind";
  return check;
}

// The layout fields the earlier firmware keyed its version 3 index on.
struct LegacyKey {
  uint32_t fileSize;
  int32_t viewportWidth;
  int32_t linesPerPage;
  int32_t fontId;
  int32_t screenMargin;
  uint8_t paragraphAlignment;
};

const LegacyKey kLegacyKey{165998, 440, 26, -1234567, 20, 2};

// The bytes the earlier firmware wrote with serialization::writePod, field by field, in host byte order.
std::vector<uint8_t> earlierFirmwareImage(const LegacyKey& key, const std::vector<uint32_t>& offsets) {
  std::vector<uint8_t> out;
  const auto put = [&out](const auto& value) {
    const size_t at = out.size();
    out.resize(at + sizeof(value));
    std::memcpy(out.data() + at, &value, sizeof(value));
  };
  put(uint32_t{0x54585449});
  put(uint8_t{3});
  put(key.fileSize);
  put(key.viewportWidth);
  put(key.linesPerPage);
  put(key.fontId);
  put(key.screenMargin);
  put(key.paragraphAlignment);
  put(static_cast<uint32_t>(offsets.size()));
  for (const uint32_t offset : offsets) put(offset);
  return out;
}

bool legacyLookup(const std::vector<uint8_t>& image, const uint32_t fileSize, const uint32_t page, uint32_t& offset,
                  const bool failReads = false) {
  return legacyPageOffset(
      [&](const size_t at, uint8_t* out, const size_t length) {
        if (failReads || at + length > image.size()) return false;
        std::memcpy(out, image.data() + at, length);
        return true;
      },
      image.size(), fileSize, page, offset);
}

}  // namespace

TEST(TxtPageIndexFormat, LittleEndianHelpersRoundTrip) {
  uint8_t bytes[4];
  writeLe32(bytes, 0x01020304u);
  EXPECT_EQ(bytes[0], 0x04);
  EXPECT_EQ(bytes[1], 0x03);
  EXPECT_EQ(bytes[2], 0x02);
  EXPECT_EQ(bytes[3], 0x01);
  EXPECT_EQ(readLe32(bytes), 0x01020304u);

  writeLe32(bytes, 0xFFFFFFFFu);
  EXPECT_EQ(readLe32(bytes), 0xFFFFFFFFu);
}

TEST(TxtPageIndexFormat, HeaderIsTheVersion4Layout) {
  uint8_t header[HEADER_BYTES];
  std::memset(header, 0xEE, sizeof(header));
  encodeHeader(header, Key{0x11223344u, 0x55667788u, 0x55, -2, 7, 20, 3, LAYOUT_CJK_PARAGRAPHS, -40}, true,
               0x0A0B0C0Du, 0x01020304u, 0xA1B2C3D4u);

  const auto revision = static_cast<uint8_t>(LAYOUT_REVISION);
  const uint8_t expected[HEADER_BYTES] = {
      0x49, 0x54,     0x58, 0x54,  // magic 0x54585449
      0x04, 0x01,     0x00, 0x00,  // version, complete, reserved
      0x44, 0x33,     0x22, 0x11,  // file size
      0x88, 0x77,     0x66, 0x55,  // source signature
      revision, 0x00, 0x00, 0x00,  // layout revision
      0x55, 0x00,     0x00, 0x00,  // viewport width
      0xFE, 0xFF,     0xFF, 0xFF,  // lines per page (-2)
      0x07, 0x00,     0x00, 0x00,  // font id
      0x14, 0x00,     0x00, 0x00,  // screen margin
      0x03, 0x01,     0xD8, 0xFF,  // alignment, layout flags, first-line indent (-40)
      0x0D, 0x0C,     0x0B, 0x0A,  // page count
      0x04, 0x03,     0x02, 0x01,  // chapter count
      0xD4, 0xC3,     0xB2, 0xA1,  // checksum
  };
  EXPECT_EQ(0, std::memcmp(header, expected, sizeof(expected)));
  EXPECT_LT(LAYOUT_REVISION, 0x100u);

  encodeHeader(header, kKey, false, 1, 0, 0);
  EXPECT_EQ(header[5], 0x00) << "a partial index";
}

TEST(TxtPageIndexFormat, CompleteIndexRoundTripsWithChapters) {
  const std::vector<uint8_t> bytes = imageOf(kKey, true, kOffsets, kChapters);
  ASSERT_EQ(bytes.size(), HEADER_BYTES + 4 * kOffsets.size() + 8 * kChapters.size());
  EXPECT_EQ(readLe32(bytes.data() + 40), kOffsets.size());
  EXPECT_EQ(readLe32(bytes.data() + 44), kChapters.size());
  EXPECT_EQ(readLe32(bytes.data() + 48), bodyChecksum(kOffsets, kChapters));
  for (size_t i = 0; i < kOffsets.size(); i++) {
    EXPECT_EQ(readLe32(bytes.data() + HEADER_BYTES + 4 * i), kOffsets[i]);
  }
  // Each heading is stored with the page it is on.
  const uint8_t* records = bytes.data() + HEADER_BYTES + 4 * kOffsets.size();
  const uint32_t pages[] = {0, 1, 3};
  for (size_t i = 0; i < kChapters.size(); i++) {
    EXPECT_EQ(readLe32(records + 8 * i), kChapters[i]);
    EXPECT_EQ(readLe32(records + 8 * i + 4), pages[i]);
  }

  Loaded loaded;
  ASSERT_EQ(readImage(bytes, kKey, loaded), Check::Ok);
  EXPECT_TRUE(loaded.complete);
  EXPECT_EQ(loaded.offsets, kOffsets);
  EXPECT_EQ(loaded.chapters, kChapters);
}

TEST(TxtPageIndexFormat, PartialIndexRoundTrips) {
  const std::vector<uint32_t> offsets{0, 1500, 3100};
  const std::vector<uint32_t> chapters{10, 3099};
  Loaded loaded;
  ASSERT_EQ(readImage(imageOf(kKey, false, offsets, chapters), kKey, loaded), Check::Ok);
  EXPECT_FALSE(loaded.complete);
  EXPECT_EQ(loaded.offsets, offsets);
  EXPECT_EQ(loaded.chapters, chapters);
}

TEST(TxtPageIndexFormat, AnEmptySourceHasTheSinglePageZero) {
  Key empty = kKey;
  empty.fileSize = 0;
  Loaded loaded;
  ASSERT_EQ(readImage(imageOf(empty, true, {0}, {}), empty, loaded), Check::Ok);
  EXPECT_EQ(loaded.offsets, std::vector<uint32_t>{0});
  EXPECT_EQ(readImage(imageOf(empty, true, {0, 1}, {}), empty), Check::BadOffsets);
}

TEST(TxtPageIndexFormat, EveryChangedKeyFieldIsNamed) {
  const std::vector<uint8_t> bytes = imageOf(kKey, true, kOffsets, kChapters);
  ASSERT_EQ(readImage(bytes, kKey), Check::Ok);

  struct Case {
    const char* name;
    void (*mutate)(Key&);
    Check expected;
  };
  const Case cases[] = {
      {"file size", [](Key& k) { k.fileSize++; }, Check::FileSizeChanged},
      {"source signature", [](Key& k) { k.sourceSignature ^= 1; }, Check::SourceChanged},
      {"viewport width", [](Key& k) { k.viewportWidth++; }, Check::LayoutChanged},
      {"lines per page", [](Key& k) { k.linesPerPage--; }, Check::LayoutChanged},
      {"font id", [](Key& k) { k.fontId = 0; }, Check::LayoutChanged},
      {"screen margin", [](Key& k) { k.screenMargin = 5; }, Check::LayoutChanged},
      {"alignment", [](Key& k) { k.paragraphAlignment = 3; }, Check::LayoutChanged},
      {"layout flags", [](Key& k) { k.layoutFlags = 0; }, Check::LayoutChanged},
      {"first-line indent", [](Key& k) { k.firstLineIndent = 0; }, Check::LayoutChanged},
  };
  for (const Case& c : cases) {
    Key changed = kKey;
    c.mutate(changed);
    EXPECT_EQ(readImage(bytes, changed), c.expected) << c.name;
  }
}

TEST(TxtPageIndexFormat, AnotherLayoutRevisionIsRebuilt) {
  for (const uint32_t revision : {LAYOUT_REVISION - 1, LAYOUT_REVISION + 1}) {
    std::vector<uint8_t> bytes = imageOf(kKey, true, kOffsets, kChapters);
    writeLe32(bytes.data() + 16, revision);
    EXPECT_EQ(readImage(bytes, kKey), Check::LayoutRevisionChanged) << revision;
  }
}

TEST(TxtPageIndexFormat, ForeignFilesAreRejected) {
  const std::vector<uint8_t> good = imageOf(kKey, true, kOffsets, kChapters);
  struct Patch {
    size_t at;
    uint8_t value;
    Check expected;
  };
  const Patch patches[] = {
      {0, 0x00, Check::BadMagic},   {4, 3, Check::BadVersion}, {4, 5, Check::BadVersion},
      {5, 2, Check::BadState},      {6, 1, Check::BadState},   {7, 1, Check::BadState},
  };
  for (const Patch& patch : patches) {
    std::vector<uint8_t> bytes = good;
    bytes[patch.at] = patch.value;
    EXPECT_EQ(readImage(bytes, kKey), patch.expected) << "byte " << patch.at << " = " << int{patch.value};
  }
}

TEST(TxtPageIndexFormat, ZeroPagesIsNotAnIndex) {
  std::vector<uint8_t> bytes(HEADER_BYTES);
  encodeHeader(bytes.data(), kKey, true, 0, 0, FNV_OFFSET);
  EXPECT_EQ(readImage(bytes, kKey), Check::BadPageCount);
}

TEST(TxtPageIndexFormat, LengthMustMatchTheCounts) {
  const std::vector<uint8_t> good = imageOf(kKey, true, kOffsets, kChapters);

  std::vector<uint8_t> longer = good;
  longer.push_back(0);
  EXPECT_EQ(readImage(longer, kKey), Check::BadLength);

  std::vector<uint8_t> shorter = good;
  shorter.pop_back();
  EXPECT_EQ(readImage(shorter, kKey), Check::BadLength);

  const std::vector<uint8_t> headerOnly(good.begin(), good.begin() + HEADER_BYTES - 1);
  EXPECT_EQ(readImage(headerOnly, kKey), Check::BadLength);

  // Counts whose byte size overflows 32 bits must not wrap around to the file length.
  uint8_t header[HEADER_BYTES];
  encodeHeader(header, kKey, true, 0xFFFFFFFFu, 0xFFFFFFFFu, 0);
  Header parsed;
  EXPECT_EQ(checkHeader(header, 100, kKey, parsed), Check::BadLength);
  encodeHeader(header, kKey, true, 0x40000000u, 0, 0);
  EXPECT_EQ(checkHeader(header, HEADER_BYTES, kKey, parsed), Check::BadLength);
}

TEST(TxtPageIndexFormat, CorruptBodiesAreCaught) {
  const std::vector<uint8_t> good = imageOf(kKey, true, kOffsets, kChapters);

  std::vector<uint8_t> offsetChanged = good;
  offsetChanged[HEADER_BYTES + 8] ^= 0x01;  // 3100 -> 3101: still a valid page list
  EXPECT_EQ(readImage(offsetChanged, kKey), Check::BadChecksum);

  std::vector<uint8_t> checksumChanged = good;
  checksumChanged[48] ^= 0x80;
  EXPECT_EQ(readImage(checksumChanged, kKey), Check::BadChecksum);

  std::vector<uint8_t> wrongPage = good;
  writeLe32(wrongPage.data() + HEADER_BYTES + 4 * kOffsets.size() + 8 + 4, 2);  // heading 1600 is on page 1
  EXPECT_EQ(readImage(wrongPage, kKey), Check::BadChapters);
}

TEST(TxtPageIndexFormat, InvalidPageListsAreRejected) {
  const std::vector<std::vector<uint32_t>> lists = {
      {5, 10}, {0, 10, 10}, {0, 20, 10}, {0, kKey.fileSize}, {0, 1500, 0xFFFFFFFFu},
  };
  for (const auto& offsets : lists) {
    EXPECT_EQ(readImage(imageOf(kKey, true, offsets, {}), kKey), Check::BadOffsets) << offsets.size();
    EXPECT_EQ(checkOffsets(offsets, kKey.fileSize), Check::BadOffsets);
  }
  EXPECT_EQ(checkOffsets({}, 10), Check::BadOffsets);
  EXPECT_EQ(checkOffsets({0}, 0), Check::Ok);
  EXPECT_EQ(checkOffsets({0}, 10), Check::Ok);
  EXPECT_EQ(checkOffsets({0, 9}, 10), Check::Ok);
}

TEST(TxtPageIndexFormat, InvalidChapterListsAreRejected) {
  EXPECT_EQ(readImage(imageOf(kKey, true, kOffsets, {1600, 1600}), kKey), Check::BadChapters);
  EXPECT_EQ(readImage(imageOf(kKey, true, kOffsets, {1600, 1500}), kKey), Check::BadChapters);
  EXPECT_EQ(readImage(imageOf(kKey, true, kOffsets, {kKey.fileSize}), kKey), Check::BadChapters);
  EXPECT_EQ(readImage(imageOf(kKey, true, kOffsets, {kKey.fileSize - 1}), kKey), Check::Ok);

  // A partial index only knows the headings on pages before its frontier.
  const std::vector<uint32_t> partial{0, 1500, 3100};
  EXPECT_EQ(readImage(imageOf(kKey, false, partial, {3099}), kKey), Check::Ok);
  EXPECT_EQ(readImage(imageOf(kKey, false, partial, {3100}), kKey), Check::BadChapters);
  EXPECT_EQ(checkChapters({3100}, partial, kKey.fileSize, true), Check::Ok);
}

TEST(TxtPageIndexFormat, FailedReadsLeaveNothingBehind) {
  const std::vector<uint8_t> bytes = imageOf(kKey, true, kOffsets, kChapters);
  for (const size_t readable : {size_t{0}, HEADER_BYTES, HEADER_BYTES + 6, bytes.size() - 1}) {
    Loaded loaded;
    EXPECT_EQ(readImage(bytes, kKey, loaded, readable), Check::ReadFailed) << readable;
    EXPECT_TRUE(loaded.offsets.empty());
    EXPECT_TRUE(loaded.chapters.empty());
  }
}

TEST(TxtPageIndexFormat, WriteFailuresAreReported) {
  std::vector<uint32_t> offsets;
  for (uint32_t i = 0; i < 100; i++) offsets.push_back(i * 100);
  for (const int failOn : {1, 2, 4}) {
    int writes = 0;
    const bool written = writeIndex([&](const uint8_t*, size_t) { return ++writes != failOn; }, kKey, true, offsets,
                                    std::vector<uint32_t>{0, 5000});
    EXPECT_FALSE(written) << failOn;
  }
}

TEST(TxtPageIndexFormat, DescriptionsNameEveryCheck) {
  const Check checks[] = {Check::Ok,          Check::BadMagic,       Check::BadVersion,
                          Check::BadState,    Check::LayoutRevisionChanged, Check::FileSizeChanged,
                          Check::SourceChanged, Check::LayoutChanged, Check::BadPageCount,
                          Check::BadLength,   Check::BadChecksum,    Check::BadOffsets,
                          Check::BadChapters, Check::ReadFailed};
  std::set<std::string> texts;
  for (const Check check : checks) {
    const std::string text = describe(check);
    EXPECT_NE(text, "unknown");
    texts.insert(text);
  }
  EXPECT_EQ(texts.size(), std::size(checks));
}

TEST(TxtPageIndexFormat, EarlierFirmwareIndexIsRecognised) {
  const std::vector<uint8_t> image = earlierFirmwareImage(kLegacyKey, kOffsets);
  ASSERT_EQ(image.size(), LEGACY_HEADER_BYTES + 4 * kOffsets.size());

  uint32_t pages = 0;
  EXPECT_TRUE(checkLegacyHeader(image.data(), image.size(), kLegacyKey.fileSize, pages));
  EXPECT_EQ(pages, kOffsets.size());
  EXPECT_FALSE(checkLegacyHeader(image.data(), image.size(), kLegacyKey.fileSize + 1, pages));
  EXPECT_EQ(pages, 0u);
  EXPECT_FALSE(checkLegacyHeader(image.data(), image.size() - 1, kLegacyKey.fileSize, pages));
  EXPECT_FALSE(checkLegacyHeader(image.data(), LEGACY_HEADER_BYTES - 1, kLegacyKey.fileSize, pages));

  const std::vector<uint8_t> noPages = earlierFirmwareImage(kLegacyKey, {});
  EXPECT_FALSE(checkLegacyHeader(noPages.data(), noPages.size(), kLegacyKey.fileSize, pages));

  // The current format is not mistaken for the earlier one, nor the other way round.
  const std::vector<uint8_t> current = imageOf(kKey, true, kOffsets, {});
  EXPECT_FALSE(checkLegacyHeader(current.data(), current.size(), kKey.fileSize, pages));
  EXPECT_EQ(readImage(image, kKey), Check::BadLength);
  std::vector<uint8_t> padded = image;
  padded.resize(HEADER_BYTES + 4 * kOffsets.size());
  EXPECT_EQ(readImage(padded, kKey), Check::BadVersion);
}

TEST(TxtPageIndexFormat, EarlierFirmwarePageBecomesAByteOffset) {
  const std::vector<uint8_t> image = earlierFirmwareImage(kLegacyKey, kOffsets);
  uint32_t offset = 0xDEAD;
  ASSERT_TRUE(legacyLookup(image, kLegacyKey.fileSize, 2, offset));
  EXPECT_EQ(offset, 3100u);
  ASSERT_TRUE(legacyLookup(image, kLegacyKey.fileSize, 0, offset));
  EXPECT_EQ(offset, 0u);
  ASSERT_TRUE(legacyLookup(image, kLegacyKey.fileSize, 9, offset)) << "a page past the end is the last page";
  EXPECT_EQ(offset, 165000u);

  offset = 77;
  EXPECT_FALSE(legacyLookup(image, kLegacyKey.fileSize - 1, 2, offset)) << "the book changed";
  EXPECT_FALSE(legacyLookup(image, kLegacyKey.fileSize, 2, offset, /*failReads=*/true));
  const std::vector<uint8_t> current = imageOf(kKey, true, kOffsets, {});
  EXPECT_FALSE(legacyLookup(current, kKey.fileSize, 2, offset));
  EXPECT_EQ(offset, 77u) << "a failed lookup leaves the offset alone";

  const std::vector<uint8_t> outside = earlierFirmwareImage(kLegacyKey, {0, kLegacyKey.fileSize + 10});
  EXPECT_FALSE(legacyLookup(outside, kLegacyKey.fileSize, 1, offset));
  ASSERT_TRUE(legacyLookup(outside, kLegacyKey.fileSize, 0, offset));
  EXPECT_EQ(offset, 0u);
}

TEST(TxtPageIndexFormat, FingerprintTracksEveryKeyField) {
  const uint32_t base = keyFingerprint(kKey);
  EXPECT_EQ(keyFingerprint(Key(kKey)), base);
  std::set<uint32_t> seen{base};
  void (*const mutations[])(Key&) = {
      [](Key& k) { k.fileSize++; },           [](Key& k) { k.sourceSignature++; },
      [](Key& k) { k.viewportWidth++; },      [](Key& k) { k.linesPerPage++; },
      [](Key& k) { k.fontId++; },             [](Key& k) { k.screenMargin++; },
      [](Key& k) { k.paragraphAlignment++; }, [](Key& k) { k.layoutFlags ^= LAYOUT_CJK_PARAGRAPHS; },
      [](Key& k) { k.firstLineIndent++; },
  };
  for (const auto mutate : mutations) {
    Key changed = kKey;
    mutate(changed);
    seen.insert(keyFingerprint(changed));
  }
  EXPECT_EQ(seen.size(), std::size(mutations) + 1);
}

TEST(TxtProgressFormat, OffsetFollowsThePageEarlierFirmwareReads) {
  uint8_t bytes[TxtProgressFormat::BYTES];
  TxtProgressFormat::encode(bytes, 0x1234, 165000, 0xDEADBEEFu);
  const uint8_t prefix[] = {0x34, 0x12, 0x00, 0x00, 'T', 'X', 'P', '2'};
  EXPECT_EQ(0, std::memcmp(bytes, prefix, sizeof(prefix)));
  EXPECT_EQ(readLe32(bytes), 0x1234u) << "earlier firmware reads the first four bytes as the page";

  const TxtProgressFormat::Position position = TxtProgressFormat::decode(bytes, sizeof(bytes));
  EXPECT_EQ(position.kind, TxtProgressFormat::Position::Kind::Offset);
  EXPECT_EQ(position.page, 0x1234u);
  EXPECT_EQ(position.offset, 165000u);
  EXPECT_EQ(position.fingerprint, 0xDEADBEEFu);
}

TEST(TxtProgressFormat, PagesBeyondSixteenBitsAreClampedButTheOffsetIsKept) {
  uint8_t bytes[TxtProgressFormat::BYTES];
  TxtProgressFormat::encode(bytes, 70000, 4000000, 1);
  const TxtProgressFormat::Position position = TxtProgressFormat::decode(bytes, sizeof(bytes));
  EXPECT_EQ(position.page, 0xFFFFu);
  EXPECT_EQ(position.offset, 4000000u);
}

TEST(TxtProgressFormat, EarlierFirmwareProgressIsAPage) {
  const uint8_t legacy[] = {7, 1, 0, 0};
  TxtProgressFormat::Position position = TxtProgressFormat::decode(legacy, sizeof(legacy));
  EXPECT_EQ(position.kind, TxtProgressFormat::Position::Kind::Page);
  EXPECT_EQ(position.page, 263u);

  uint8_t unknownTail[TxtProgressFormat::BYTES] = {9, 0, 0, 0, 'X', 'X', 'X', 'X'};
  position = TxtProgressFormat::decode(unknownTail, sizeof(unknownTail));
  EXPECT_EQ(position.kind, TxtProgressFormat::Position::Kind::Page);
  EXPECT_EQ(position.page, 9u);

  EXPECT_EQ(TxtProgressFormat::decode(legacy, 3).kind, TxtProgressFormat::Position::Kind::None);
  EXPECT_EQ(TxtProgressFormat::decode(legacy, 0).kind, TxtProgressFormat::Position::Kind::None);
}
