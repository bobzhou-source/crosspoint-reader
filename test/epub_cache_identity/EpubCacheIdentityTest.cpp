#include <BookCacheUtils.h>
#include <Epub.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
constexpr const char* CACHE_ROOT = "/.crosspoint";

// The contract uses this platform's std::hash, not an ESP32-specific hash value.
std::string expectedCache(const std::string& sourcePath) {
  return std::string(CACHE_ROOT) + "/epub_" + std::to_string(std::hash<std::string>{}(sourcePath));
}

class EpubCacheIdentityTest : public testing::TestWithParam<std::string> {
 protected:
  void SetUp() override {
    std::string name = (fs::temp_directory_path() / "epub-cache-identity-XXXXXX").string();
    const char* directory = ::mkdtemp(name.data());
    ASSERT_NE(directory, nullptr);
    sandbox = directory;
    Storage.root = sandbox;
    Storage.removalAttempts.clear();
    Storage.failRemoval = false;
  }

  void TearDown() override {
    Storage.root.clear();
    Storage.removalAttempts.clear();
    Storage.failRemoval = false;
    if (!sandbox.empty()) {
      std::error_code error;
      fs::remove_all(sandbox, error);
      EXPECT_FALSE(error) << error.message();
    }
  }

  void seedCache(const std::string& directory, const std::string& contents) {
    const auto root = Storage.resolve(directory.c_str());
    ASSERT_TRUE(fs::create_directories(root / "sections"));
    std::ofstream metadata(root / "book.bin", std::ios::binary);
    metadata << contents;
    metadata.close();
    ASSERT_TRUE(metadata.good());
    std::ofstream section(root / "sections" / "section.bin", std::ios::binary);
    section << "nested " << contents;
    section.close();
    ASSERT_TRUE(section.good());
  }

  std::string metadata(const std::string& directory) const {
    std::ifstream file(Storage.resolve(directory.c_str()) / "book.bin", std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }

 private:
  fs::path sandbox;
};

TEST_P(EpubCacheIdentityTest, DefaultAndExplicitNativeIdentityAgree) {
  const std::string path = GetParam();
  const Epub implicit(path, CACHE_ROOT);
  const Epub explicitPath(path, CACHE_ROOT, path);
  const Epub explicitEmpty(path, CACHE_ROOT, std::string{});

  EXPECT_EQ(implicit.getPath(), path);
  EXPECT_EQ(explicitPath.getPath(), path);
  EXPECT_EQ(explicitEmpty.getPath(), path);
  EXPECT_EQ(implicit.getCachePath(), expectedCache(path));
  EXPECT_EQ(implicit.getCachePath(), explicitPath.getCachePath());
  EXPECT_EQ(implicit.getCachePath(), explicitEmpty.getCachePath());
  EXPECT_NE(implicit.getCachePath(), expectedCache(""));
  EXPECT_EQ(path, GetParam());
}

TEST_P(EpubCacheIdentityTest, MovedInputRetainsItsOriginalIdentity) {
  std::string path = GetParam();
  const Epub book(std::move(path), CACHE_ROOT);
  EXPECT_EQ(book.getPath(), GetParam());
  EXPECT_EQ(book.getCachePath(), expectedCache(GetParam()));
}

TEST_P(EpubCacheIdentityTest, DistinctNativePathsDoNotShareEmptyIdentity) {
  const std::string otherPath = "/other" + GetParam();
  const Epub selected(GetParam(), CACHE_ROOT);
  const Epub other(otherPath, CACHE_ROOT);
  EXPECT_EQ(other.getPath(), otherPath);
  EXPECT_EQ(other.getCachePath(), expectedCache(otherPath));
  EXPECT_NE(selected.getCachePath(), other.getCachePath());
}

TEST_P(EpubCacheIdentityTest, ConvertedDocumentUsesOriginalSourceOverride) {
  const std::string source = GetParam() + ".md";
  const std::string convertedPath = "/.crosspoint/converted/document.epub";
  const Epub converted(convertedPath, CACHE_ROOT, source);
  const Epub regenerated("/.crosspoint/converted/rebuilt/document.epub", CACHE_ROOT, source);
  const Epub native(convertedPath, CACHE_ROOT);

  EXPECT_EQ(converted.getPath(), source);
  EXPECT_EQ(converted.getCachePath(), expectedCache(source));
  EXPECT_EQ(converted.getCachePath(), regenerated.getCachePath());
  EXPECT_NE(converted.getCachePath(), native.getCachePath());
}

TEST_P(EpubCacheIdentityTest, ClearBookCacheRemovesOnlySelectedReadersCache) {
  // Reader supplies explicit source identity. Home/sleep/cache-clear callers
  // default it. Seed through the reader's identity, then use the actual utility.
  const Epub reader(GetParam(), CACHE_ROOT, GetParam());
  const std::string otherPath = "/other" + GetParam();
  const Epub otherReader(otherPath, CACHE_ROOT, otherPath);
  const std::string legacyEmptyCache = expectedCache("");
  ASSERT_NE(reader.getCachePath(), otherReader.getCachePath());
  ASSERT_NE(reader.getCachePath(), legacyEmptyCache);
  ASSERT_NE(otherReader.getCachePath(), legacyEmptyCache);
  seedCache(reader.getCachePath(), "selected book");
  seedCache(otherReader.getCachePath(), "other book");
  seedCache(legacyEmptyCache, "legacy shared sentinel");

  clearBookCache(GetParam());

  EXPECT_FALSE(Storage.exists(reader.getCachePath().c_str()));
  EXPECT_EQ(metadata(otherReader.getCachePath()), "other book");
  EXPECT_EQ(metadata(legacyEmptyCache), "legacy shared sentinel");
  EXPECT_TRUE(fs::exists(Storage.resolve(otherReader.getCachePath().c_str()) / "sections" / "section.bin"));
  EXPECT_TRUE(fs::exists(Storage.resolve(legacyEmptyCache.c_str()) / "sections" / "section.bin"));
  EXPECT_EQ(Storage.removalAttempts, std::vector<std::string>{reader.getCachePath()});
}

TEST_P(EpubCacheIdentityTest, MissingSelectedCachePreservesOtherAndLegacyCaches) {
  const std::string otherCache = expectedCache("/other" + GetParam());
  const std::string legacyEmptyCache = expectedCache("");
  seedCache(otherCache, "other book");
  seedCache(legacyEmptyCache, "legacy shared sentinel");

  clearBookCache(GetParam());
  EXPECT_TRUE(Epub(GetParam(), CACHE_ROOT).clearCache());

  EXPECT_TRUE(Storage.removalAttempts.empty());
  EXPECT_EQ(metadata(otherCache), "other book");
  EXPECT_EQ(metadata(legacyEmptyCache), "legacy shared sentinel");
}

TEST_P(EpubCacheIdentityTest, RemovalFailureIsReportedWithoutDeletingOtherCaches) {
  const Epub book(GetParam(), CACHE_ROOT);
  const std::string legacyEmptyCache = expectedCache("");
  seedCache(expectedCache(GetParam()), "selected book");
  seedCache(legacyEmptyCache, "legacy shared sentinel");
  Storage.failRemoval = true;

  EXPECT_FALSE(book.clearCache());

  EXPECT_EQ(Storage.removalAttempts, std::vector<std::string>{expectedCache(GetParam())});
  EXPECT_EQ(metadata(expectedCache(GetParam())), "selected book");
  EXPECT_EQ(metadata(legacyEmptyCache), "legacy shared sentinel");
}

INSTANTIATE_TEST_SUITE_P(
    NativePaths, EpubCacheIdentityTest,
    testing::Values("/a.epub", "/Books/" + std::string(128, 'x') + "/Long native document title.epub",
                    "/书库/繁體中文.EPUB"),
    ([](const testing::TestParamInfo<std::string>& info) {
      constexpr const char* names[] = {"Short", "Long", "UnicodeUppercaseExtension"};
      return names[info.index];
    }));
}  // namespace
