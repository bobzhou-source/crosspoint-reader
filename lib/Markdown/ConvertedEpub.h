#pragma once

#include <HalStorage.h>
#include <Print.h>

#include <cstdint>
#include <string>
#include <string_view>

// Streams a document into an EPUB understood by the native reader. The original
// path remains the reader/cache identity; this file is only its transport.
class ConvertedEpub {
 public:
  enum class Preparation { Cached, Build, Failed };

  ConvertedEpub(const std::string& sourcePath, const std::string& cacheRoot, const char* formatTag,
                uint32_t formatVersion);
  ~ConvertedEpub();
  Preparation prepare();
  bool begin();
  Print& content() { return body; }
  bool good() const;
  void setTitle(std::string_view title);
  void setAuthor(std::string_view author);
  void setLanguage(std::string_view language);
  bool addTocEntry(std::string_view title, std::string_view anchor);
  bool addImage(const std::string& sourcePath, const char* itemName, const char* mediaType);
  bool beginResource(const char* itemName, const char* mediaType);
  Print& resource() { return zipData; }
  bool endResource();
  bool finish();
  const std::string& path() const { return archivePath; }
  static bool writeXml(Print& output, std::string_view text);

 private:
  class BufferedFile final : public Print {
   public:
    HalFile file;
    bool ok = true;
    uint32_t crc = 0;
    uint32_t length = 0;
    size_t write(uint8_t value) override { return write(&value, 1); }
    size_t write(const uint8_t* data, size_t size) override;
    bool flushBuffer();
    bool close();
    void resetEntry();

   private:
    uint8_t buffer[1024];
    size_t used = 0;
  };

  std::string sourcePath;
  std::string cachePath;
  std::string archivePath;
  uint32_t formatVersion;
  uint32_t formatHash;
  uint32_t sourceModified = 0;
  uint8_t transferBuffer[1024];
  uint64_t sourceHash = 0;
  uint32_t sourceSize = 0;
  BufferedFile body;
  BufferedFile toc;
  BufferedFile manifest;
  BufferedFile zipData;
  HalFile central;
  char title[512] = {};
  char author[256] = {};
  char language[48] = "en";
  char entryName[192] = {};
  uint32_t entryOffset = 0;
  uint32_t tocCount = 0;
  uint16_t entryCount = 0;
  bool entryOpen = false;
  bool started = false;
  bool failed = false;

  bool startEntry(const char* name);
  bool finishEntry();
  bool copyFile(const std::string& filePath, Print& output);
  bool invalidateLayout();
  bool fingerprint(uint32_t& size, uint32_t& modified, uint64_t& hash);
  bool writePackage();
  bool writeDirectory();
  bool fail(const char* reason);
};
