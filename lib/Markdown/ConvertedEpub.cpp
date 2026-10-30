#include "ConvertedEpub.h"

#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <functional>

namespace {
constexpr uint32_t CACHE_MAGIC = 0x31564344;  // DCV1
constexpr const char* TEMP_FILES[] = {"/convert.body.tmp", "/convert.toc.tmp", "/convert.manifest.tmp",
                                     "/convert.zip.tmp", "/convert.central.tmp", "/convert.meta.tmp"};

uint32_t crc32(uint32_t crc, const uint8_t* data, size_t size) {
  static constexpr uint32_t TABLE[] = {0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
                                       0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
                                       0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
  crc = ~crc;
  while (size--) {
    crc ^= *data++;
    crc = (crc >> 4) ^ TABLE[crc & 15];
    crc = (crc >> 4) ^ TABLE[crc & 15];
  }
  return ~crc;
}

bool put16(Print& out, uint16_t n) {
  const uint8_t bytes[] = {static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8)};
  return out.write(bytes, sizeof(bytes)) == sizeof(bytes);
}

bool put32(Print& out, uint32_t n) {
  return put16(out, n & 0xffff) && put16(out, n >> 16);
}

bool literal(Print& out, const char* text) {
  const size_t size = strlen(text);
  return out.write(reinterpret_cast<const uint8_t*>(text), size) == size;
}

void copyText(char* out, size_t capacity, std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\n' || text.front() == '\r')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1);
  size_t length = std::min(text.size(), capacity - 1);
  if (length < text.size()) {
    while (length && (static_cast<uint8_t>(text[length]) & 0xc0) == 0x80) --length;
  }
  if (length) memcpy(out, text.data(), length);
  out[length] = 0;
}

bool safeEntry(const char* name) {
  return name && *name && *name != '/' && strlen(name) < 192 && !strstr(name, "..") && !strchr(name, '\\') &&
         !strchr(name, ':');
}
}  // namespace

ConvertedEpub::ConvertedEpub(const std::string& source, const std::string& root, const char* tag, uint32_t version)
    : sourcePath(source),
      cachePath(root + "/epub_" + std::to_string(std::hash<std::string>{}(source))),
      archivePath(cachePath + "/converted.epub"),
      formatVersion(version),
      formatHash(crc32(0, reinterpret_cast<const uint8_t*>(tag), strlen(tag))) {
  const size_t slash = source.find_last_of('/');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  const size_t dot = source.find_last_of('.');
  setTitle(std::string_view(source).substr(start, dot != std::string::npos && dot > start ? dot - start : source.size()));
}

ConvertedEpub::~ConvertedEpub() {
  body.close();
  toc.close();
  manifest.close();
  zipData.close();
  if (central && !central.close()) failed = true;
  if (started) {
    for (const char* suffix : TEMP_FILES) Storage.remove((cachePath + suffix).c_str());
  }
}

size_t ConvertedEpub::BufferedFile::write(const uint8_t* data, size_t size) {
  if (!ok || size > UINT32_MAX - length) {
    ok = false;
    return 0;
  }
  const size_t requested = size;
  crc = crc32(crc, data, size);
  length += size;
  while (size) {
    const size_t count = std::min(size, sizeof(buffer) - used);
    memcpy(buffer + used, data, count);
    used += count;
    size -= count;
    data += count;
    if (used == sizeof(buffer) && !flushBuffer()) return 0;
  }
  return requested;
}

bool ConvertedEpub::BufferedFile::flushBuffer() {
  if (used && ok) {
    ok = file.position() <= UINT32_MAX - used && file.write(buffer, used) == used;
  }
  used = 0;
  return ok;
}

bool ConvertedEpub::BufferedFile::close() {
  flushBuffer();
  if (file && !file.close()) ok = false;
  return ok;
}

void ConvertedEpub::BufferedFile::resetEntry() {
  crc = 0;
  length = 0;
}

bool ConvertedEpub::fail(const char* reason) {
  LOG_ERR("CONVERT", "%s", reason);
  failed = true;
  return false;
}

bool ConvertedEpub::good() const { return !failed && body.ok && toc.ok && manifest.ok && zipData.ok; }

bool ConvertedEpub::fingerprint(uint32_t& size, uint32_t& modified, uint64_t& hash) {
  HalFile source;
  if (!Storage.openFileForRead("CONVERT", sourcePath, source) || source.isDirectory() ||
      source.fileSize64() > UINT32_MAX) return fail("Cannot fingerprint source");
  size = source.size();
  modified = source.modificationTime();
  hash = 14695981039346656037ULL;
  uint32_t read = 0;
  while (read < size) {
    const int count = source.read(transferBuffer, std::min<uint32_t>(sizeof(transferBuffer), size - read));
    if (count <= 0) return fail("Source read failed");
    for (int i = 0; i < count; ++i) hash = (hash ^ transferBuffer[i]) * 1099511628211ULL;
    read += count;
    if ((read & 0x3fff) == 0) vTaskDelay(1);
  }
  return source.size() == size && source.modificationTime() == modified;
}

ConvertedEpub::Preparation ConvertedEpub::prepare() {
  if (!fingerprint(sourceSize, sourceModified, sourceHash)) return Preparation::Failed;
  HalFile marker;
  HalFile archive;
  uint32_t data[8] = {};
  if (Storage.exists((cachePath + "/converted.meta").c_str()) &&
      Storage.openFileForRead("CONVERT", cachePath + "/converted.meta", marker) && marker.size() == sizeof(data) &&
      marker.read(data, sizeof(data)) == sizeof(data) && data[0] == CACHE_MAGIC && data[1] == formatVersion &&
      data[2] == formatHash && data[3] == sourceSize && data[4] == static_cast<uint32_t>(sourceHash) &&
      data[5] == static_cast<uint32_t>(sourceHash >> 32) && data[7] == sourceModified &&
      Storage.openFileForRead("CONVERT", archivePath, archive) && archive.size() == data[6] && data[6] > 22) {
    return Preparation::Cached;
  }
  return Preparation::Build;
}

bool ConvertedEpub::begin() {
  started = true;
  if (!Storage.ensureDirectoryExists(cachePath.c_str()) ||
      !Storage.openFileForWrite("CONVERT", cachePath + TEMP_FILES[0], body.file) ||
      !Storage.openFileForWrite("CONVERT", cachePath + TEMP_FILES[1], toc.file) ||
      !Storage.openFileForWrite("CONVERT", cachePath + TEMP_FILES[2], manifest.file) ||
      !Storage.openFileForWrite("CONVERT", cachePath + TEMP_FILES[3], zipData.file) ||
      !Storage.openFileForWrite("CONVERT", cachePath + TEMP_FILES[4], central)) {
    return fail("Cannot create conversion files");
  }
  if (!startEntry("mimetype") || !literal(zipData, "application/epub+zip") || !finishEntry() ||
      !startEntry("META-INF/container.xml") ||
      !literal(zipData, "<?xml version=\"1.0\"?><container version=\"1.0\" "
                        "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
                        "<rootfile full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/>"
                        "</rootfiles></container>") ||
      !finishEntry()) return fail("Cannot write EPUB container");
  return literal(body, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                       "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">"
                       "<head><title>") &&
         writeXml(body, title) && literal(body, "</title></head><body>");
}

void ConvertedEpub::setTitle(std::string_view value) { copyText(title, sizeof(title), value); }
void ConvertedEpub::setAuthor(std::string_view value) { copyText(author, sizeof(author), value); }
void ConvertedEpub::setLanguage(std::string_view value) { copyText(language, sizeof(language), value); }

bool ConvertedEpub::writeXml(Print& output, std::string_view text) {
  for (size_t i = 0; i < text.size();) {
    const uint8_t c = text[i];
    const char* escaped = nullptr;
    switch (c) {
      case '&': escaped = "&amp;"; break;
      case '<': escaped = "&lt;"; break;
      case '>': escaped = "&gt;"; break;
      case '"': escaped = "&quot;"; break;
      case '\'': escaped = "&apos;"; break;
      default: break;
    }
    if (escaped) {
      if (!literal(output, escaped)) return false;
      ++i;
      continue;
    }
    size_t count = 1;
    uint32_t cp = c;
    bool valid = c == 9 || c == 10 || c == 13 || (c >= 32 && c < 128);
    if (c >= 0xc2 && c <= 0xf4) {
      count = c < 0xe0 ? 2 : (c < 0xf0 ? 3 : 4);
      cp = c & (0x7f >> count);
      valid = i + count <= text.size();
      for (size_t j = 1; valid && j < count; ++j) {
        const uint8_t next = text[i + j];
        valid = (next & 0xc0) == 0x80;
        cp = (cp << 6) | (next & 0x3f);
      }
      valid = valid && cp >= (count == 2 ? 0x80U : count == 3 ? 0x800U : 0x10000U) && cp <= 0x10ffff &&
              !(cp >= 0xd800 && cp <= 0xdfff) && cp != 0xfffe && cp != 0xffff;
    }
    if (!valid) {
      if (!literal(output, "\xef\xbf\xbd")) return false;
      ++i;
    } else {
      if (output.write(reinterpret_cast<const uint8_t*>(text.data() + i), count) != count) return false;
      i += count;
    }
  }
  return true;
}

bool ConvertedEpub::addTocEntry(std::string_view label, std::string_view anchor) {
  if (tocCount == UINT16_MAX) return fail("Too many contents entries");
  char number[24];
  snprintf(number, sizeof(number), "%lu", static_cast<unsigned long>(++tocCount));
  return literal(toc, "<navPoint id=\"n") && literal(toc, number) && literal(toc, "\" playOrder=\"") &&
         literal(toc, number) && literal(toc, "\"><navLabel><text>") && writeXml(toc, label) &&
         literal(toc, "</text></navLabel><content src=\"book.xhtml") &&
         (anchor.empty() || (literal(toc, "#") && writeXml(toc, anchor))) && literal(toc, "\"/></navPoint>");
}

bool ConvertedEpub::startEntry(const char* name) {
  if (!good() || entryOpen || entryCount == UINT16_MAX || !safeEntry(name) || !zipData.flushBuffer()) {
    return fail("Invalid EPUB resource");
  }
  if (zipData.file.position() > UINT32_MAX - 65536) return fail("EPUB exceeds ZIP32 size");
  entryOffset = zipData.file.position();
  strcpy(entryName, name);
  // Data descriptors permit streaming without seeking or buffering a resource.
  const bool ok = put32(zipData.file, 0x04034b50) && put16(zipData.file, 20) && put16(zipData.file, 8) &&
                  put16(zipData.file, 0) && put32(zipData.file, 0) && put32(zipData.file, 0) &&
                  put32(zipData.file, 0) && put32(zipData.file, 0) && put16(zipData.file, strlen(name)) &&
                  put16(zipData.file, 0) && literal(zipData.file, name);
  zipData.resetEntry();
  entryOpen = ok;
  return ok || fail("ZIP header write failed");
}

bool ConvertedEpub::finishEntry() {
  if (!entryOpen || !zipData.flushBuffer()) return fail("ZIP resource write failed");
  const bool ok = put32(zipData.file, 0x08074b50) && put32(zipData.file, zipData.crc) &&
                  put32(zipData.file, zipData.length) && put32(zipData.file, zipData.length) &&
                  put32(central, 0x02014b50) && put16(central, 20) && put16(central, 20) && put16(central, 8) &&
                  put16(central, 0) && put32(central, 0) && put32(central, zipData.crc) &&
                  put32(central, zipData.length) && put32(central, zipData.length) &&
                  put16(central, strlen(entryName)) && put16(central, 0) && put16(central, 0) &&
                  put16(central, 0) && put16(central, 0) && put32(central, 0) && put32(central, entryOffset) &&
                  literal(central, entryName);
  entryOpen = false;
  ++entryCount;
  return ok || fail("ZIP directory write failed");
}

bool ConvertedEpub::beginResource(const char* name, const char* mediaType) {
  if (!safeEntry(name) || !mediaType) return fail("Invalid resource name/type");
  if (!strcmp(name, "mimetype") || !strcmp(name, "book.xhtml") || !strcmp(name, "content.opf") ||
      !strcmp(name, "toc.ncx") || !strcmp(name, "META-INF/container.xml")) return fail("Reserved resource name");
  if (!startEntry(name)) return false;
  char id[24];
  snprintf(id, sizeof(id), "r%u", entryCount);
  return literal(manifest, "<item id=\"") && literal(manifest, id) && literal(manifest, "\" href=\"") &&
         writeXml(manifest, name) && literal(manifest, "\" media-type=\"") && writeXml(manifest, mediaType) &&
         literal(manifest, "\"/>");
}

bool ConvertedEpub::endResource() { return finishEntry(); }

bool ConvertedEpub::copyFile(const std::string& filePath, Print& output) {
  HalFile input;
  if (!Storage.openFileForRead("CONVERT", filePath, input)) return fail("Cannot open conversion input");
  uint32_t remaining = input.size();
  uint32_t chunks = 0;
  while (remaining) {
    const int count = input.read(transferBuffer, std::min<uint32_t>(remaining, sizeof(transferBuffer)));
    if (count <= 0 || output.write(transferBuffer, count) != static_cast<size_t>(count)) return fail("Resource I/O failed");
    remaining -= count;
    if (++chunks % 16 == 0) vTaskDelay(1);
  }
  return true;
}

bool ConvertedEpub::addImage(const std::string& source, const char* name, const char* type) {
  return beginResource(name, type) && copyFile(source, zipData) && endResource();
}

bool ConvertedEpub::writePackage() {
  if (!startEntry("book.xhtml") || !copyFile(cachePath + TEMP_FILES[0], zipData) || !finishEntry() ||
      !startEntry("toc.ncx") ||
      !literal(zipData, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><ncx "
                        "xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\"><head/>"
                        "<docTitle><text>") ||
      !writeXml(zipData, title) || !literal(zipData, "</text></docTitle><navMap>") ||
      !copyFile(cachePath + TEMP_FILES[1], zipData) || !literal(zipData, "</navMap></ncx>") || !finishEntry() ||
      !startEntry("content.opf") ||
      !literal(zipData, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><package "
                        "xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\" unique-identifier=\"uid\">"
                        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>") ||
      !writeXml(zipData, title) || !literal(zipData, "</dc:title><dc:creator>") || !writeXml(zipData, author) ||
      !literal(zipData, "</dc:creator><dc:language>") || !writeXml(zipData, language) ||
      !literal(zipData, "</dc:language><dc:identifier id=\"uid\">") || !writeXml(zipData, sourcePath) ||
      !literal(zipData, "</dc:identifier></metadata><manifest>"
                        "<item id=\"book\" href=\"book.xhtml\" media-type=\"application/xhtml+xml\"/>"
                        "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>") ||
      !copyFile(cachePath + TEMP_FILES[2], zipData) ||
      !literal(zipData, "</manifest><spine toc=\"ncx\"><itemref idref=\"book\"/></spine>"
                        "<guide><reference type=\"text\" title=\"" ) ||
      !writeXml(zipData, title) || !literal(zipData, "\" href=\"book.xhtml\"/></guide></package>") ||
      !finishEntry()) return fail("EPUB package write failed");
  return true;
}

bool ConvertedEpub::writeDirectory() {
  if (!central.close()) return fail("Cannot close ZIP directory");
  const uint32_t offset = zipData.file.position();
  if (!copyFile(cachePath + TEMP_FILES[4], zipData.file)) return false;
  const uint32_t size = zipData.file.position() - offset;
  return put32(zipData.file, 0x06054b50) && put16(zipData.file, 0) && put16(zipData.file, 0) &&
         put16(zipData.file, entryCount) && put16(zipData.file, entryCount) && put32(zipData.file, size) &&
         put32(zipData.file, offset) && put16(zipData.file, 0);
}

bool ConvertedEpub::invalidateLayout() {
  HalFile directory = Storage.open(cachePath.c_str());
  if (!directory) return fail("Cannot invalidate layout cache");
  while (true) {
    HalFile file = directory.openNextFile();
    if (!file) break;
    char name[192];
    file.getName(name, sizeof(name));
    const bool isDir = file.isDirectory();
    const bool derived = !strcmp(name, "book.bin") || !strcmp(name, "sections") ||
                         !strncmp(name, "css_rules.", 10) || !strncmp(name, "cover", 5) ||
                         !strncmp(name, "thumb_", 6) || !strncmp(name, ".cover", 6);
    if (!derived) continue;
    file.close();  // Close before deleting the cached entry.
    const std::string target = cachePath + "/" + name;
    if (!(isDir ? Storage.removeDir(target.c_str()) : Storage.remove(target.c_str()))) {
      return fail("Cannot remove stale layout cache");
    }
  }
  return true;
}

bool ConvertedEpub::finish() {
  if (!started || entryOpen || !good() || !literal(body, "</body></html>") ||
      (!tocCount && !addTocEntry(title, "")) || !body.close() || !toc.close() || !manifest.close() ||
      !writePackage() || !writeDirectory() || !zipData.flushBuffer()) return fail("Conversion did not complete");
  const uint32_t archiveSize = zipData.file.position();
  uint32_t size, modified;
  uint64_t hash;
  if (!fingerprint(size, modified, hash) || size != sourceSize || modified != sourceModified || hash != sourceHash) {
    return fail("Source changed during conversion");
  }
  if (!zipData.close() || !invalidateLayout()) return false;
  if (Storage.exists(archivePath.c_str()) && !Storage.remove(archivePath.c_str())) return fail("Cannot replace EPUB");
  if (!Storage.rename((cachePath + TEMP_FILES[3]).c_str(), archivePath.c_str())) return fail("Cannot publish EPUB");
  const uint32_t data[] = {CACHE_MAGIC, formatVersion, formatHash, sourceSize, static_cast<uint32_t>(sourceHash),
                           static_cast<uint32_t>(sourceHash >> 32), archiveSize, sourceModified};
  {
    HalFile marker;
    if (!Storage.openFileForWrite("CONVERT", cachePath + TEMP_FILES[5], marker) ||
        marker.write(data, sizeof(data)) != sizeof(data)) return fail("Cannot write conversion marker");
    marker.flush();
  }
  const std::string marker = cachePath + "/converted.meta";
  if (Storage.exists(marker.c_str()) && !Storage.remove(marker.c_str())) return fail("Cannot replace conversion marker");
  if (!Storage.rename((cachePath + TEMP_FILES[5]).c_str(), marker.c_str())) return fail("Cannot publish conversion marker");
  return true;
}
