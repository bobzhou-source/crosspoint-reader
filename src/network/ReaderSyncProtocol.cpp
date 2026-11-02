#include "ReaderSyncProtocol.h"
#include "ReaderSyncCanonical.h"

#include <Memory.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

namespace reader_sync {
namespace {
constexpr char PAGE[] = "/.crosspoint/reader-sync/page.tmp";
constexpr char DIRTY[] = "/.crosspoint/library.dirty";
constexpr size_t BUFFER_BYTES = 8193;
constexpr size_t PAGE_BYTES = 160 * 1024;

// Filtered page documents are bounded even if the server sends hostile JSON.
// Prefer external RAM on S3; C3 falls back to bounded ordinary heap.
class JsonBudget final : public ArduinoJson::Allocator {
  size_t used = 0;
  static constexpr size_t LIMIT = 64 * 1024;
  struct alignas(std::max_align_t) Header { size_t size; };
 public:
  void* allocate(size_t n) override {
    if (n > LIMIT - used) return nullptr;
    void* p = nullptr;
#ifdef ESP_PLATFORM
    p = heap_caps_malloc(n + sizeof(Header), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!p) p = malloc(n + sizeof(Header));
    if (!p) return nullptr;
    static_cast<Header*>(p)->size = n;
    used += n;
    return static_cast<Header*>(p) + 1;
  }
  void deallocate(void* p) override {
    if (!p) return;
    auto* h = static_cast<Header*>(p) - 1;
    used -= h->size;
    free(h);
  }
  void* reallocate(void* p, size_t n) override {
    if (!p) return allocate(n);
    const size_t old = (static_cast<Header*>(p) - 1)->size;
    if (n <= old) return p;
    void* next = allocate(n);
    if (next) { memcpy(next, p, old); deallocate(p); }
    return next;
  }
};

bool hex(const char* text, size_t n) {
  if (!text || strlen(text) != n) return false;
  for (size_t i = 0; i < n; ++i) if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
  return true;
}
bool uuid(const char* text) {
  if (!text || strlen(text) != 36) return false;
  for (size_t i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (text[i] != '-') return false; }
    else if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
  }
  return true;
}
std::string hexBytes(const uint8_t* bytes, size_t count) {
  static constexpr char DIGITS[] = "0123456789abcdef";
  std::string out(count * 2, '0');
  for (size_t i = 0; i < count; ++i) { out[i * 2] = DIGITS[bytes[i] >> 4]; out[i * 2 + 1] = DIGITS[bytes[i] & 15]; }
  return out;
}
std::string digest(const uint8_t* bytes, size_t count) {
  uint8_t hash[32];
  mbedtls_sha256(bytes, count, hash, 0);
  return hexBytes(hash, sizeof(hash));
}
std::string signature(const std::string& key, const std::string& message) {
  uint8_t hash[32];
  if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                     reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                     reinterpret_cast<const uint8_t*>(message.data()), message.size(), hash) != 0) return {};
  static constexpr char TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve(43);
  uint32_t bits = 0;
  unsigned count = 0;
  for (const auto byte : hash) {
    bits = (bits << 8) | byte;
    count += 8;
    while (count >= 6) { count -= 6; out.push_back(TABLE[(bits >> count) & 63]); }
  }
  if (count) out.push_back(TABLE[(bits << (6 - count)) & 63]);
  return out;
}
struct FileReader {
  HalFile& file;
  int read() { return file.read(); }
  size_t readBytes(char* bytes, size_t count) { const int n = file.read(bytes, count); return n > 0 ? static_cast<size_t>(n) : 0; }
};
bool basename(const char* source, const char* id, std::string& name) {
  if (!source) { name = std::string(id) + ".epub"; return true; }
  if (strlen(source) > 4096) return false;
  const char* slash = strrchr(source, '/');
  name = slash ? slash + 1 : source;
  if (name.size() < 6 || name.size() > 240 || name.front() == '.' || name.substr(name.size() - 5) != ".epub") return false;
  for (const unsigned char c : name) if (c < 0x20 || c == 0x7f || strchr("\\:*?\"<>|", c)) return false;
  return true;
}
bool epubHeader(const uint8_t* p, size_t n) {
  // EPUB's first ZIP entry must be unencrypted and stored, with exact mimetype.
  return n >= 58 && memcmp(p, "PK\003\004", 4) == 0 && (p[6] & 9) == 0 && p[7] == 0 &&
         p[8] == 0 && p[9] == 0 && p[18] == 20 && p[19] == 0 && p[20] == 0 && p[21] == 0 &&
         p[22] == 20 && p[23] == 0 && p[24] == 0 && p[25] == 0 && p[26] == 8 && p[27] == 0 &&
         p[28] == 0 && p[29] == 0 && memcmp(p + 30, "mimetypeapplication/epub+zip", 28) == 0;
}
}  // namespace

Protocol::Protocol(const Config& config, Cancel cancel, void* context, Clock clock, Random random,
                   void (*invalidateCache)(const std::string&))
    : config(config), cancel(cancel), context(context), clock(clock), random(random), invalidateCache(invalidateCache),
      buffer(makeUniqueNoThrow<uint8_t[]>(BUFFER_BYTES)) {
  http.setTimeout(4000);
  http.setFollowRedirects(0);
  // Signed nonces cannot be transparently replayed after a keep-alive race.
  http.setRetryReusedConnection(false);
  http.setReuse(true);
  http.setUserAgent("ReaderSend/1");
}
bool Protocol::cancelled() const { return cancel && cancel(context); }

bool Protocol::prepare(const char* method, const std::string& route, const std::string& body) {
  const int64_t now = clock();
  if (now < 1704067200LL || now > 4102444800LL) { result.phase = Phase::ClockError; return false; }
  uint8_t randomBytes[16];
  random(randomBytes, sizeof(randomBytes));
  const std::string nonce = hexBytes(randomBytes, sizeof(randomBytes));
  const std::string stamp = std::to_string(now);
  const std::string canonical = std::string("reader-sync-v1\n") + method + "\n" + route + "\n" +
      digest(reinterpret_cast<const uint8_t*>(body.data()), body.size()) + "\n" + stamp + "\n" + nonce;
  if (!http.begin(config.origin + route)) { result.phase = Phase::InvalidConfig; return false; }
  http.addHeader("Authorization", "Bearer " + config.key);
  http.addHeader("X-Reader-Timestamp", stamp);
  http.addHeader("X-Reader-Nonce", nonce);
  http.addHeader("X-Reader-Signature", signature(config.key, canonical));
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json, application/epub+zip");
  return true;
}
int Protocol::request(const char* method, const std::string& route, const std::string& body,
                      const freeink::SecureHttpClient::DataCallback& sink) {
  if (cancelled()) { result.phase = Phase::Cancelled; return -1; }
  if (!prepare(method, route, body)) return -1;
  result.bytesSent += static_cast<uint32_t>(body.size());
  // Counts every delivered response byte, including bytes a sink then rejects.
  return http.sendRequest(method, reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                          [&](const uint8_t* bytes, size_t n) {
                            result.bytesReceived += static_cast<uint32_t>(n);
                            return sink ? sink(bytes, n) : true;
                          },
                          [this] { return cancelled(); });
}
bool Protocol::responseOk(int code, int alternate) {
  if (cancelled()) { result.phase = Phase::Cancelled; return false; }
  if ((code == 200 || code == alternate) && http.responseComplete()) return true;
  if (code == 401 || code == 403) result.phase = Phase::AuthError;
  else if (code == 429) {
    result.phase = Phase::RateLimited;
    const unsigned long seconds = strtoul(http.getHeader("retry-after").c_str(), nullptr, 10);
    result.retrySeconds = static_cast<uint32_t>(std::min(86400UL, std::max(60UL, seconds)));
  } else if (code == 410) result.phase = Phase::NeedsRedelivery;
  else if (code == 503) { result.phase = Phase::ProtocolError; result.parkUntilWake = true; }
  else if (code < 0) result.phase = Phase::Offline;
  else result.phase = Phase::ProtocolError;
  return false;
}

bool Protocol::readJson(const std::string& route, JsonDocument& doc, bool list) {
  auto file = Storage.open(PAGE, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) { result.phase = Phase::StorageError; return false; }
  size_t received = 0;
  bool ioError = false;
  const int code = request("GET", route, {}, [&](const uint8_t* bytes, size_t n) {
    if (received + n > PAGE_BYTES) return false;
    received += n;
    if (file.write(bytes, n) != n) { ioError = true; return false; }
    return true;
  });
  const bool synced = file.sync();
  file.close();
  if (ioError || !synced) { result.phase = Phase::StorageError; Storage.remove(PAGE); return false; }
  // A removed cursor earns exactly one head restart, performed by run().
  if (code == 400 && !result.after.empty()) result.retrySeconds = 1;
  if (!responseOk(code)) { Storage.remove(PAGE); return false; }
  file = Storage.open(PAGE);
  FileReader reader{file};
  JsonDocument filter;
  if (list) {
    filter["items"][0]["id"] = true;
    filter["items"][0]["state"] = true;
    filter["items"][0]["sha256"] = true;
    filter["items"][0]["size"] = true;
    filter["items"][0]["source_path"] = true;
    filter["has_more"] = true;
    filter["next_after"] = true;
  }
  const auto err = list ? deserializeJson(doc, reader, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(5))
                        : deserializeJson(doc, reader, DeserializationOption::NestingLimit(5));
  file.close();
  Storage.remove(PAGE);
  if (err || doc.overflowed() || !doc.is<JsonObject>()) { result.phase = Phase::ProtocolError; return false; }
  return true;
}

bool Protocol::hashFile(const std::string& path, size_t size, const char* expected) {
  auto file = Storage.open(path.c_str());
  if (!file || file.size() != size) return false;
  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash);
  mbedtls_sha256_starts(&hash, 0);
  size_t total = 0;
  bool ok = true;
  while (total < size) {
    if (cancelled()) { ok = false; break; }
    const int n = file.read(buffer.get(), std::min(size - total, BUFFER_BYTES));
    if (n <= 0 || (total == 0 && !epubHeader(buffer.get(), n))) { ok = false; break; }
    mbedtls_sha256_update(&hash, buffer.get(), n);
    total += n;
  }
  uint8_t bytes[32];
  mbedtls_sha256_finish(&hash, bytes);
  mbedtls_sha256_free(&hash);
  return ok && total == size && hexBytes(bytes, 32) == expected;
}

bool Protocol::install(JsonVariantConst item) {
  const char* id = item["id"].as<const char*>();
  const char* sha = item["sha256"].as<const char*>();
  if (!item.is<JsonObjectConst>() || !uuid(id) || !hex(sha, 64) || item["state"] != "ready" ||
      !item["size"].is<size_t>() || (!item["source_path"].isNull() && !item["source_path"].is<const char*>())) {
    result.phase = Phase::ProtocolError; return false;
  }
  const size_t size = item["size"].as<size_t>();
  std::string name;
  if (size < 58 || size > MAX_BOOK_BYTES || !basename(item["source_path"].as<const char*>(), id, name)) {
    result.phase = Phase::ProtocolError; return false;
  }
  const std::string ledger = std::string(PRIVATE_ROOT) + "/items/" + id + ".json";
  std::string destination;
  bool existing = false;
  // A ledger is advisory: its target is always rehashed before an ACK.
  auto entry = Storage.open(ledger.c_str());
  if (entry && entry.size() <= 1024) {
    FileReader reader{entry};
    JsonDocument doc;
    if (!deserializeJson(doc, reader)) {
      const char* path = doc["path"].as<const char*>();
      if (path && strncmp(path, "/books/Inbox/", 13) == 0 && !privateWebPath(path) &&
          hashFile(path, size, sha)) { destination = path; existing = true; }
    }
  }
  entry.close();
  for (unsigned rung = 0; destination.empty() && rung < 3; ++rung) {
    std::string candidate = "/books/Inbox/";
    if (rung == 0) candidate += name;
    else if (rung == 1) candidate += name.substr(0, name.size() - 5) + "-" + std::string(sha, 8) + ".epub";
    else candidate += std::string(id) + ".epub";
    if (!Storage.exists(candidate.c_str())) destination = candidate;
    else if (hashFile(candidate, size, sha)) { destination = candidate; existing = true; }
    if (cancelled()) { result.phase = Phase::Cancelled; return false; }
  }
  if (destination.empty()) { result.phase = Phase::StorageError; return false; }
  const std::string partial = std::string(PRIVATE_ROOT) + "/" + id + ".part";
  if (!existing) {
    const uint64_t available = Storage.freeBytes();
    if (available < size + 64 * 1024) { result.phase = Phase::StorageError; return false; }
    auto file = Storage.open(partial.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    if (!file) { result.phase = Phase::StorageError; return false; }
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    mbedtls_sha256_starts(&hash, 0);
    size_t received = 0;
    bool ioError = false;
    bool headersChecked = false;
    const int code = request("GET", std::string("/reader/items/") + id + "/content", {}, [&](const uint8_t* bytes, size_t n) {
      if (!headersChecked) {
        if (http.getHeader("content-type") != "application/epub+zip" || http.getHeader("x-reader-sha256") != sha) return false;
        headersChecked = true;
      }
      if (n > size - received) return false;
      if (file.write(bytes, n) != n) { ioError = true; return false; }
      mbedtls_sha256_update(&hash, bytes, n);
      received += n;
      return true;
    });
    uint8_t actual[32];
    mbedtls_sha256_finish(&hash, actual);
    mbedtls_sha256_free(&hash);
    const bool synced = file.sync();
    const bool closed = file.close();
    const bool accepted = responseOk(code);
    if (!accepted || ioError || !synced || !closed || received != size || hexBytes(actual, 32) != sha ||
        !hashFile(partial, size, sha) || cancelled()) {
      Storage.remove(partial.c_str());
      if (ioError || !synced || !closed) result.phase = Phase::StorageError;
      else if (accepted) result.phase = cancelled() ? Phase::Cancelled : Phase::ProtocolError;
      return false;
    }
    // No user file is ever replaced. The private partial and final live on one volume.
    if (Storage.exists(destination.c_str()) || !Storage.rename(partial.c_str(), destination.c_str()) ||
        !hashFile(destination, size, sha)) {
      Storage.remove(partial.c_str()); result.phase = Phase::StorageError; return false;
    }
    ++result.books;
    invalidateCache(destination);
  } else Storage.remove(partial.c_str());
  JsonDocument receipt;
  receipt["id"] = id;
  receipt["sha256"] = sha;
  receipt["size"] = size;
  receipt["state"] = "pending";
  receipt["path"] = destination;
  std::string serialized;
  serializeJson(receipt, serialized);
  if (receipt.overflowed() || !atomicWrite(ledger.c_str(), reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size())) {
    result.phase = Phase::StorageError; return false;
  }
  // The marker itself must be durable before ACK, not merely an in-memory hint.
  auto marker = Storage.open(DIRTY, O_WRONLY | O_CREAT);
  const bool marked = marker && marker.sync();
  const bool closed = marker.close();
  if (!marked || !closed) { result.phase = Phase::StorageError; return false; }
  receipt["state"] = "complete";
  serialized.clear();
  serializeJson(receipt, serialized);
  if (receipt.overflowed() || !atomicWrite(ledger.c_str(), reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size())) {
    result.phase = Phase::StorageError; return false;
  }
  size_t responseBytes = 0;
  const int code = request("POST", std::string("/reader/items/") + id + "/ack", {}, [&](const uint8_t*, size_t n) {
    responseBytes += n; return responseBytes <= 16384;
  });
  return responseOk(code);
}

bool Protocol::uploadNote(const std::string& path) {
  auto file = Storage.open(path.c_str());
  if (!file || file.size() == 0 || file.size() > 8192) { ++result.rejectedNotes; return true; }
  const size_t size = file.size();
  if (file.read(buffer.get(), size) != static_cast<int>(size)) { result.phase = Phase::StorageError; return false; }
  file.close();
  std::string record, id, error;
  if (!canonicalAnnotation(reinterpret_cast<const char*>(buffer.get()), size, config.deviceId.c_str(), clock(), record, id, error)) {
    ++result.rejectedNotes; return true;
  }
  const std::string hash = digest(reinterpret_cast<const uint8_t*>(record.data()), record.size());
  const std::string receipt = std::string(PRIVATE_ROOT) + "/notes/" + id + ".receipt";
  auto prior = Storage.open(receipt.c_str());
  if (prior && prior.size() == 64 && prior.read(buffer.get(), 64) == 64 && memcmp(buffer.get(), hash.data(), 64) == 0) return true;
  prior.close();
  const std::string rejected = std::string(PRIVATE_ROOT) + "/notes/" + id + ".rejected";
  prior = Storage.open(rejected.c_str());
  if (prior && prior.size() == 64 && prior.read(buffer.get(), 64) == 64 && memcmp(buffer.get(), hash.data(), 64) == 0) {
    ++result.rejectedNotes;
    return true;
  }
  prior.close();
  const std::string body = "{\"annotations\":[" + record + "]}";
  if (body.size() > 65536) { ++result.rejectedNotes; return true; }
  size_t responseBytes = 0;
  ++noteAttempts;
  const int code = request("POST", "/reader/annotations", body, [&](const uint8_t* data, size_t n) {
    if (responseBytes + n > 1024) return false;
    memcpy(buffer.get() + responseBytes, data, n); responseBytes += n; return true;
  });
  if (code == 400 || code == 409 || code == 413 || code == 415 || code == 422) {
    ++result.rejectedNotes;
    if (!atomicWrite(rejected.c_str(), reinterpret_cast<const uint8_t*>(hash.data()), hash.size())) {
      result.phase = Phase::StorageError;
      return false;
    }
    return true;
  }
  if (!responseOk(code, 201)) return false;
  JsonDocument accepted;
  if (deserializeJson(accepted, buffer.get(), responseBytes) || accepted["accepted"] != 1 ||
      !accepted["inserted"].is<unsigned>() || accepted["inserted"].as<unsigned>() > 1) {
    result.phase = Phase::ProtocolError; return false;
  }
  if (!atomicWrite(receipt.c_str(), reinterpret_cast<const uint8_t*>(hash.data()), hash.size())) {
    result.phase = Phase::StorageError; return false;
  }
  ++result.notes;
  return true;
}

bool Protocol::uploadNotes() {
  constexpr char ROOT[] = "/.crosspoint/annotations";
  constexpr char CURSOR[] = "/.crosspoint/reader-sync/note-cursor";
  if (!Storage.exists(ROOT)) return true;
  char resume[26]{};
  {
    auto prior = Storage.open(CURSOR);
    if (prior && prior.size() == 25 && prior.read(reinterpret_cast<uint8_t*>(resume), 25) != 25) {
      result.phase = Phase::StorageError; return false;
    }
  }
  auto checkpoint = [&](const char* key) {
    if (atomicWrite(CURSOR, reinterpret_cast<const uint8_t*>(key), strlen(key))) return true;
    result.phase = Phase::StorageError;
    return false;
  };
  // Bound actual record/hash work, not total library size. A durable cursor lets
  // later native records progress instead of silently rejecting everything after a cap.
  for (unsigned pass = 0; pass < 2; ++pass) {
    auto root = Storage.open(ROOT);
    if (!root || !root.isDirectory()) { result.phase = Phase::StorageError; return false; }
    bool found = resume[0] == '\0';
    unsigned seen = 0;
    for (auto dir = root.openNextFile(); dir; dir = root.openNextFile()) {
      if (cancelled()) { result.phase = Phase::Cancelled; return false; }
      char source[32];
      dir.getName(source, sizeof(source));
      if (!dir.isDirectory() || !hex(source, 16)) continue;
      for (auto note = dir.openNextFile(); note; note = dir.openNextFile()) {
        if (cancelled()) { result.phase = Phase::Cancelled; return false; }
        char name[32];
        note.getName(name, sizeof(name));
        // Native records only; do not import companion bundles or private receipts.
        if (note.isDirectory() || strlen(name) != 8 || strcmp(name + 3, ".json") != 0 ||
            name[0] < '0' || name[0] > '9' || name[1] < '0' || name[1] > '9' || name[2] < '0' || name[2] > '9') continue;
        char key[26];
        snprintf(key, sizeof(key), "%s/%s", source, name);
        if (!found) {
          found = strcmp(resume, key) == 0;
          continue;
        }
        note.close();
        if (!uploadNote(std::string(ROOT) + "/" + key)) return false;
        if (++seen == 256 || noteAttempts == 8) {
          result.more = true;
          return checkpoint(key);
        }
      }
    }
    if (found) return checkpoint("");
    // The cursor's native file was deleted: restart at the head once. UUID receipts
    // make replay safe, and the next checkpoint repairs the vanished cursor.
    resume[0] = '\0';
  }
  return true;
}

Result Protocol::run(const std::string& after) {
  result = Result{};
  noteAttempts = 0;
  if (!buffer) { result.phase = Phase::MemoryError; return result; }
  if (!Storage.ensureDirectoryExists(PRIVATE_ROOT) || !Storage.ensureDirectoryExists("/.crosspoint/reader-sync/items") ||
      !Storage.ensureDirectoryExists("/.crosspoint/reader-sync/notes") || !Storage.ensureDirectoryExists("/books/Inbox")) {
    result.phase = Phase::StorageError; return result;
  }
  // Only private interrupted payloads are discarded; user books are never cleanup targets.
  {
    auto directory = Storage.open(PRIVATE_ROOT);
    unsigned count = 0;
    for (auto entry = directory.openNextFile(); entry && count < 256; entry = directory.openNextFile()) {
      if (cancelled()) { result.phase = Phase::Cancelled; return result; }
      char name[48];
      entry.getName(name, sizeof(name));
      if (entry.isDirectory() || strlen(name) != 41 || strcmp(name + 36, ".part") != 0) continue;
      name[36] = '\0';
      if (!uuid(name)) continue;
      name[36] = '.';
      entry.close();
      if (!Storage.remove((std::string(PRIVATE_ROOT) + "/" + name).c_str())) {
        result.phase = Phase::StorageError; return result;
      }
      ++count;
    }
  }
  result.after = after;
  JsonBudget budget;
  JsonDocument page(&budget);
  std::string route = "/reader/items?limit=8";
  if (!after.empty() && uuid(after.c_str())) route += "&after=" + after;
  bool loaded = readJson(route, page, true);
  if (!loaded && result.retrySeconds == 1 && !after.empty()) {
    result.retrySeconds = 0; result.after.clear(); result.phase = Phase::Success;
    loaded = readJson("/reader/items?limit=8", page, true);
  }
  if (!loaded) return result;
  if (!page["items"].is<JsonArray>() || page["items"].size() > 8 || !page["has_more"].is<bool>()) {
    result.phase = Phase::ProtocolError; return result;
  }
  const bool more = page["has_more"].as<bool>();
  const char* next = page["next_after"].as<const char*>();
  if ((more && (!uuid(next) || page["items"].size() == 0 || page["items"][page["items"].size() - 1]["id"] != next)) ||
      (!more && !page["next_after"].isNull())) { result.phase = Phase::ProtocolError; return result; }
  for (JsonVariantConst item : page["items"].as<JsonArrayConst>()) if (!install(item)) return result;
  result.after = more ? next : "";
  result.more = more;
  page.clear();
  if (!uploadNotes()) return result;
  if (result.rejectedNotes) result.phase = Phase::AnnotationError;
  return result;
}
}  // namespace reader_sync
