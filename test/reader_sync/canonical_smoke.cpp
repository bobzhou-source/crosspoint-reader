// Host cross-language smoke for reader_sync::canonicalAnnotation().
//
// Fixtures are written with the real ArduinoJson native writer (the same calls
// AnnotationStore::writeEntry makes), then canonicalized by the production
// implementation and reported base64-encoded so the Python oracle can recompute
// the expected record with the companion's own map_annotation()/json_bytes().
// Extra cases arrive on stdin as one JSON object per line:
//   {"name":"...", "raw":"<json text>"} or {"name":"...", "raw_b64":"<bytes>"}
// with optional "device" and "now" overrides.
//
// Build and run through canonical_oracle.py; see that file for the command.

#include <ArduinoJson.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "ReaderSyncCanonical.h"

#if defined(__has_include)
#  if __has_include(<version>)
#    include <version>
#  endif
#endif
// Must mirror the selection in ReaderSyncCanonical.cpp so the reported
// implementation matches the compiled one.
#if defined(READER_SYNC_PRINTF_FLOAT)
#  define SMOKE_USE_TO_CHARS 0
#elif defined(READER_SYNC_USE_TO_CHARS)
#  define SMOKE_USE_TO_CHARS READER_SYNC_USE_TO_CHARS
#elif defined(__cpp_lib_to_chars) || defined(_LIBCPP_VERSION)
#  define SMOKE_USE_TO_CHARS 1
#else
#  define SMOKE_USE_TO_CHARS 0
#endif
#if SMOKE_USE_TO_CHARS
#  define SMOKE_FLOAT_IMPL "to_chars"
#else
#  define SMOKE_FLOAT_IMPL "printf"
#endif

namespace {

constexpr char kDevice[] = "6A4B4C73-B747-33DA-BF3D-455A3C03D0CB";
constexpr int64_t kNow = 1767225600;  // 2026-01-01T00:00:00Z

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string encodeBase64(const char* data, size_t size) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(data);
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (size_t i = 0; i < size; i += 3) {
    const uint32_t a = bytes[i];
    const uint32_t b = i + 1 < size ? bytes[i + 1] : 0;
    const uint32_t c = i + 2 < size ? bytes[i + 2] : 0;
    const uint32_t triple = (a << 16) | (b << 8) | c;
    out.push_back(kBase64[(triple >> 18) & 0x3f]);
    out.push_back(kBase64[(triple >> 12) & 0x3f]);
    out.push_back(i + 1 < size ? kBase64[(triple >> 6) & 0x3f] : '=');
    out.push_back(i + 2 < size ? kBase64[triple & 0x3f] : '=');
  }
  return out;
}

int decodeBase64Char(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool decodeBase64(const std::string& text, std::string& out) {
  out.clear();
  uint32_t buffer = 0;
  int bits = 0;
  for (const char c : text) {
    if (c == '=' || c == '\n' || c == '\r') continue;
    const int value = decodeBase64Char(c);
    if (value < 0) return false;
    buffer = (buffer << 6) | static_cast<uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xff));
    }
  }
  return true;
}

void report(const std::string& name, const std::string& raw, const std::string& device, int64_t now) {
  std::string record;
  std::string uuid;
  std::string error;
  const bool ok =
      reader_sync::canonicalAnnotation(raw.data(), raw.size(), device.c_str(), now, record, uuid, error);
  JsonDocument view;
  const bool ajAccepts = deserializeJson(view, raw.data(), raw.size()) == DeserializationError::Ok;
  JsonDocument line;
  line["name"] = name;
  line["raw_b64"] = encodeBase64(raw.data(), raw.size());
  line["device"] = device;
  line["now"] = now;
  line["ok"] = ok;
  line["aj_accepts"] = ajAccepts;
  line["record"] = record;
  line["uuid"] = uuid;
  line["error"] = error;
  std::string text;
  serializeJson(line, text);
  text.push_back('\n');
  std::fwrite(text.data(), 1, text.size(), stdout);
}

// The same shape AnnotationStore::writeEntry produces.
std::string nativeRecord(const char* sourcePath, const char* sourceTitle, const char* chapterTitle,
                         const char* chapterHref, const char* xpath, float percentage, int si, int pc, int pp,
                         bool hasVo, uint32_t vo, const char* excerpt, const char* note, bool pageContext,
                         uint64_t createdAt, int kind) {
  JsonDocument doc;
  doc["kind"] = kind;
  doc["version"] = 1;
  doc["id"] = 7;
  doc["sourcePath"] = sourcePath;
  doc["sourceTitle"] = sourceTitle;
  doc["chapterTitle"] = chapterTitle;
  doc["chapterHref"] = chapterHref;
  doc["xpath"] = xpath;
  doc["percentage"] = percentage;
  doc["si"] = si;
  doc["pc"] = pc;
  doc["pp"] = pp;
  if (hasVo) doc["vo"] = vo;
  doc["excerpt"] = excerpt;
  doc["note"] = note;
  doc["pageContext"] = pageContext;
  doc["createdAt"] = createdAt;
  std::string out;
  out.reserve(measureJson(doc) + 1);
  serializeJson(doc, out);
  return out;
}

void writeArduinoFixtures() {
  report("aj/plain",
         nativeRecord("Books/novel.epub", "A Novel", "Chapter One", "ch01.xhtml", "/body/div[2]/p[7]", 0.4237f,
                      3, 10, 4, false, 0, "ordinary words", "a thought", false, kNow, 0),
         kDevice, kNow);
  report("aj/vo-zero",
         nativeRecord("Books/novel.epub", "A Novel", "Chapter One", "ch01.xhtml", "/body/div[2]/p[7]", 0.0f, 0, 0,
                      0, true, 0, "", "", true, kNow, 2),
         kDevice, kNow);
  report("aj/emoji",
         nativeRecord("Books/中文 书名.epub", "书名 📚 café", "第一章", "ch01.xhtml", "/body/div/p[1]", 1.0f, 1, 1, 1,
                      true, 9, "这是摘录 — “quoted”\n\ttabbed", "note 备注 🎉", true, kNow - 3600, 1),
         kDevice, kNow);
  report("aj/later",
         nativeRecord("Books/essay.epub", "Essay", "", "", "/body/p[1]", 0.99f, 12, 40, 39, true, 65535,
                      "excerpt", "later", false, kNow - 60, 2),
         kDevice, kNow);

  {
    JsonDocument doc;
    doc["kind"] = 0;
    doc["version"] = 1;
    doc["id"] = 128;
    doc["sourcePath"] = "Books/unknown.epub";
    doc["sourceTitle"] = "Unknown fields";
    doc["chapterTitle"] = "C";
    doc["chapterHref"] = "c.xhtml";
    doc["xpath"] = "/body/p[1]";
    doc["percentage"] = 0.000001f;
    doc["si"] = 0;
    doc["pc"] = 1;
    doc["pp"] = 0;
    doc["excerpt"] = "e";
    doc["note"] = "n";
    doc["pageContext"] = true;
    doc["createdAt"] = kNow;
    JsonObject extras = doc["extras"].to<JsonObject>();
    extras["zeta"] = 1;
    extras["alpha"] = "x";
    JsonArray list = extras["list"].to<JsonArray>();
    list.add(1);
    list.add(2.5f);
    list.add(true);
    list.add(nullptr);
    doc["extraFlag"] = false;
    std::string raw;
    raw.reserve(measureJson(doc) + 1);
    serializeJson(doc, raw);
    report("aj/unknown-fields", raw, kDevice, kNow);
  }
}

}  // namespace

int main() {
  {
    JsonDocument meta;
    meta["kind"] = "meta";
    meta["float_impl"] = SMOKE_FLOAT_IMPL;
    std::string text;
    serializeJson(meta, text);
    text.push_back('\n');
    std::fwrite(text.data(), 1, text.size(), stdout);
  }

  writeArduinoFixtures();

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    JsonDocument envelope;
    if (deserializeJson(envelope, line)) {
      std::fprintf(stderr, "bad envelope: %s\n", line.c_str());
      return 2;
    }
    std::string name = envelope["name"] | "";
    std::string raw;
    if (!envelope["raw_b64"].isNull()) {
      const std::string encoded = envelope["raw_b64"].as<const char*>();
      if (!decodeBase64(encoded, raw)) {
        std::fprintf(stderr, "bad base64 for %s\n", name.c_str());
        return 2;
      }
    } else {
      const char* text = envelope["raw"] | "";
      raw.assign(text, std::strlen(text));
    }
    const std::string device = envelope["device"] | kDevice;
    const int64_t now = envelope["now"] | kNow;
    report(name, raw, device, now);
  }
  return 0;
}
