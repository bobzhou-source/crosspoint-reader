#include "BenchmarkFixtures.h"

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace benchmark_fixtures {
namespace {

static constexpr char FIXTURE_DIRECTORY[] = "/.crosspoint/benchmark/fixtures-v1";
static constexpr char EPUB_PATH[] = "/.crosspoint/benchmark/fixtures-v1/reading.epub";
static constexpr char TXT_PATH[] = "/.crosspoint/benchmark/fixtures-v1/notes.txt";
static constexpr char BITMAP_PATH[] = "/.crosspoint/benchmark/fixtures-v1/paper.bmp";
static constexpr size_t IO_CHUNK_SIZE = 128;

#include "ReadingFixture.inc"
#include "NotesFixture.inc"
#include "PaperFixture.inc"

static_assert(sizeof(READING_EPUB_BYTES) == 6490);
static_assert(sizeof(NOTES_TXT_BYTES) == 165998);
static_assert(sizeof(PAPER_BMP_BYTES) == 48062);

struct Fixture {
  const char* path;
  const uint8_t* bytes;
  size_t size;
};

static constexpr Fixture FIXTURES[] = {
    {EPUB_PATH, READING_EPUB_BYTES, sizeof(READING_EPUB_BYTES)},
    {TXT_PATH, NOTES_TXT_BYTES, sizeof(NOTES_TXT_BYTES)},
    {BITMAP_PATH, PAPER_BMP_BYTES, sizeof(PAPER_BMP_BYTES)},
};

PrepareResult verifyExisting(HalFile& file, const Fixture& fixture) {
  if (file.size() != fixture.size) return PrepareResult::Conflict;

  uint8_t buffer[IO_CHUNK_SIZE];
  size_t offset = 0;
  while (offset < fixture.size) {
    const size_t remaining = fixture.size - offset;
    const size_t chunkSize = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    const int bytesRead = file.read(buffer, chunkSize);
    if (bytesRead != static_cast<int>(chunkSize) ||
        std::memcmp(buffer, fixture.bytes + offset, chunkSize) != 0) {
      return PrepareResult::Conflict;
    }
    offset += chunkSize;
  }
  return PrepareResult::Ready;
}

PrepareResult prepareFixture(const Fixture& fixture) {
  {
    HalFile existing = Storage.open(fixture.path, O_RDONLY);
    if (existing) return verifyExisting(existing, fixture);
  }

  bool created = false;
  bool completed = false;
  {
    HalFile file = Storage.open(fixture.path, O_WRONLY | O_CREAT | O_EXCL);
    if (!file) {
      HalFile raced = Storage.open(fixture.path, O_RDONLY);
      if (raced) return verifyExisting(raced, fixture);
      return Storage.exists(fixture.path) ? PrepareResult::Conflict : PrepareResult::WriteFailed;
    }
    created = true;

    size_t offset = 0;
    while (offset < fixture.size) {
      const size_t remaining = fixture.size - offset;
      const size_t chunkSize = remaining < IO_CHUNK_SIZE ? remaining : IO_CHUNK_SIZE;
      if (file.write(fixture.bytes + offset, chunkSize) != chunkSize) break;
      offset += chunkSize;
    }
    const bool wroteAll = offset == fixture.size;
    const bool synced = wroteAll && file.sync();
    const bool closed = file.close();
    completed = wroteAll && synced && closed;
  }

  if (!completed && created) Storage.remove(fixture.path);
  return completed ? PrepareResult::Ready : PrepareResult::WriteFailed;
}

}  // namespace

PrepareResult prepare() {
  if (!Storage.ready() || !Storage.ensureDirectoryExists(FIXTURE_DIRECTORY)) {
    return PrepareResult::StorageUnavailable;
  }

  for (const Fixture& fixture : FIXTURES) {
    const PrepareResult result = prepareFixture(fixture);
    if (result != PrepareResult::Ready) return result;
  }
  return PrepareResult::Ready;
}

const char* epubPath() { return EPUB_PATH; }
const char* txtPath() { return TXT_PATH; }
const char* bitmapPath() { return BITMAP_PATH; }

}  // namespace benchmark_fixtures
