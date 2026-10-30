// Copyright (c) 2026 Pavel Liashkov. MIT License; see ../LICENSE.
// CrossPoint transport adaptation of PapyriX's LocalSend receiver.
#include "LocalsendServer.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <esp_random.h>
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
constexpr char TAG[] = "LSEND";
constexpr size_t BODY_OFFSET = 192;
constexpr uint32_t ANNOUNCE_INTERVAL_MS = 3000;
const IPAddress MULTICAST_GROUP(224, 0, 0, 167);

// Arena lifetime is one parse; deallocation and shrink never fragment the heap.
class JsonArena final : public ArduinoJson::Allocator {
 public:
  JsonArena(uint8_t* data, size_t capacity) : data(data), capacity(capacity) {}
  void* allocate(size_t size) override {
    constexpr size_t ALIGN = alignof(std::max_align_t);
    const size_t start = (used + sizeof(size_t) + ALIGN - 1) & ~(ALIGN - 1);
    if (start > capacity || size > capacity - start) return nullptr;
    memcpy(data + start - sizeof(size_t), &size, sizeof(size));
    used = start + size;
    return data + start;
  }
  void deallocate(void*) override {}
  void* reallocate(void* old, size_t size) override {
    if (!old) return allocate(size);
    size_t oldSize;
    memcpy(&oldSize, static_cast<uint8_t*>(old) - sizeof(size_t), sizeof(oldSize));
    if (size <= oldSize) return old;
    void* next = allocate(size);
    if (next) memcpy(next, old, oldSize);
    return next;
  }

 private:
  uint8_t* data;
  size_t capacity;
  size_t used = 0;
};

// ArduinoJson's stream adapter leaves a cursor, allowing rejection of trailing
// garbage rather than silently accepting a valid JSON prefix.
class JsonBodyReader {
 public:
  JsonBodyReader(const char* data, size_t size) : data(data), size(size) {}
  int read() { return offset < size ? static_cast<unsigned char>(data[offset++]) : -1; }
  size_t readBytes(char* out, size_t count) {
    count = std::min(count, size - offset);
    memcpy(out, data + offset, count);
    offset += count;
    return count;
  }
  bool finished() {
    while (offset < size) {
      const char c = data[offset++];
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
    }
    return true;
  }

 private:
  const char* data;
  size_t size;
  size_t offset = 0;
};

bool isPartialName(const char* name) {
  constexpr char PREFIX[] = ".localsend-";
  constexpr size_t PREFIX_LEN = sizeof(PREFIX) - 1;
  if (strlen(name) != PREFIX_LEN + 16 + 5 || strncmp(name, PREFIX, PREFIX_LEN) != 0 ||
      strcmp(name + PREFIX_LEN + 16, ".part") != 0) return false;
  for (size_t i = PREFIX_LEN; i < PREFIX_LEN + 16; i++) {
    if (!(name[i] >= '0' && name[i] <= '9') && !(name[i] >= 'a' && name[i] <= 'f')) return false;
  }
  return true;
}

bool validJsonString(JsonVariantConst value) {
  if (!value.is<const char*>()) return false;
  const JsonString text = value.as<JsonString>();
  return text.c_str() && strlen(text.c_str()) == text.size();
}
}  // namespace

bool LocalsendServer::begin() {
  stop();
  status = {};
  if (WiFi.status() != WL_CONNECTED) {
    setError(Error::Network);
    return false;
  }
  if (!Storage.ready() || !Storage.ensureDirectoryExists(RECEIVE_DIR) || !removeStalePartials()) {
    setError(Error::FileStorage);
    return false;
  }
  for (size_t i = 0; i < 4; i++) {
    snprintf(fingerprint + i * 8, 9, "%08lx", static_cast<unsigned long>(esp_random()));
  }
  service.begin("CrossPoint", fingerprint, PORT, &esp_random);
  listener.begin();
  if (!listener) {
    setError(Error::Network);
    listener.end();
    return false;
  }
  listener.setNoDelay(true);
  if (!discovery.beginMulticast(MULTICAST_GROUP, PORT)) {
    LOG_ERR(TAG, "Multicast join failed; listener stopped");
    listener.end();
    setError(Error::Network);
    return false;
  }
  running = true;
  announce(true);
  lastAnnounce = millis();
  lastDiscoveryReply = 0;
  announcesLeft = 2;
  LOG_INF(TAG, "HTTP receiver on port %u; heap %u", PORT, static_cast<unsigned>(ESP.getFreeHeap()));
  return true;
}

bool LocalsendServer::removeStalePartials() {
  HalFile directory = Storage.open(RECEIVE_DIR);
  if (!directory || !directory.isDirectory()) return false;
  for (;;) {
    HalFile file = directory.openNextFile();
    if (!file) break;
    char name[64];
    file.getName(name, sizeof(name));
    if (file.isDirectory() || !isPartialName(name)) continue;
    char path[80];
    snprintf(path, sizeof(path), "%s/%s", RECEIVE_DIR, name);
    file.close();  // Close before removing an interrupted transfer.
    if (!Storage.remove(path)) {
      LOG_ERR(TAG, "Cannot remove stale partial %s", path);
      return false;
    }
  }
  return true;
}

void LocalsendServer::stop() {
  for (size_t i = 0; i < CLIENT_COUNT; i++) closeConnection(i);
  listener.end();
  discovery.stop();
  service.endSession();
  jsonOwner = -1;
  running = false;
  status.receiving = false;
}

void LocalsendServer::setError(Error error) {
  status.error = error;
  ++status.revision;
  LOG_ERR(TAG, "Receiver error %u", static_cast<unsigned>(error));
}

void LocalsendServer::announce(bool initial) {
  const size_t size = service.buildAnnounce(reinterpret_cast<char*>(io), sizeof(io), initial);
  if (!size || !discovery.beginPacket(MULTICAST_GROUP, PORT)) return;
  discovery.write(io, size);
  if (!discovery.endPacket()) LOG_ERR(TAG, "Discovery announcement failed");
}

void LocalsendServer::pollDiscovery() {
  const int size = discovery.parsePacket();
  if (size <= 0) return;
  const int read = discovery.read(io, sizeof(io));
  if (read != size || size >= static_cast<int>(sizeof(io))) return;
  JsonArena arena(jsonArena, sizeof(jsonArena));
  JsonDocument document(&arena);
  if (deserializeJson(document, io, size, DeserializationOption::NestingLimit(4))) return;
  if (!document["announce"].is<bool>() || !document["announce"].as<bool>()) return;
  const char* sender = document["fingerprint"] | "";
  if (!sender[0] || strcmp(sender, fingerprint) == 0) return;
  // A fallback multicast response is specified by LocalSend v2. It avoids
  // opening outbound TLS sockets just to discover a sender.
  const uint32_t now = millis();
  if (lastDiscoveryReply && now - lastDiscoveryReply < 250) return;
  lastDiscoveryReply = now;
  announce(false);
}

void LocalsendServer::poll() {
  if (!running) return;
  if (WiFi.status() != WL_CONNECTED) {
    stop();
    setError(Error::Network);
    return;
  }
  pollDiscovery();
  if (announcesLeft && millis() - lastAnnounce >= ANNOUNCE_INTERVAL_MS) {
    announce(true);
    --announcesLeft;
    lastAnnounce = millis();
  }
  for (size_t i = 0; i < CLIENT_COUNT; i++) {
    auto& connection = connections[i];
    if (connection.phase != Phase::Idle) continue;
    WiFiClient client = listener.available();
    if (!client) break;
    connection.client = client;
    connection.client.setNoDelay(true);
    connection.client.setTimeout(1);
    snprintf(connection.peer, sizeof(connection.peer), "%s", client.remoteIP().toString().c_str());
    connection.startedAt = connection.lastActivity = millis();
    connection.phase = Phase::Request;
    break;
  }
  for (size_t i = 0; i < CLIENT_COUNT; i++) pollConnection(i);
  updateProgress();
}

void LocalsendServer::pollConnection(size_t index) {
  auto& c = connections[index];
  if (c.phase == Phase::Idle) return;
  const uint32_t now = millis();
  if (now - c.lastActivity > STALL_MS ||
      (!c.entry && c.phase != Phase::Reply && now - c.startedAt > CONTROL_TIMEOUT_MS)) {
    if (c.phase == Phase::Reply) closeConnection(index);
    else fail(index, 408, Error::Transfer);
    return;
  }
  if (!c.client.connected() && c.client.available() == 0) {
    if (c.entry) setError(Error::Transfer);
    closeConnection(index);
    return;
  }
  if (c.phase == Phase::Reply || c.phase == Phase::Continue) {
    const size_t remaining = c.responseSize - c.responseSent;
    const size_t sent = c.client.write(reinterpret_cast<const uint8_t*>(c.response + c.responseSent),
                                       std::min(remaining, size_t{1024}));
    if (!sent) return;
    c.lastActivity = now;
    c.responseSent += sent;
    if (c.responseSent != c.responseSize) return;
    if (c.phase == Phase::Reply) closeConnection(index);
    else c.phase = Phase::Body;
    return;
  }
  if (c.phase == Phase::Body) {
    readBody(index);
    return;
  }
  // A bounded header slice leaves time for Back, cancel, Wi-Fi and watchdogs.
  for (size_t bytes = 0; bytes < 1024 && c.client.available() > 0; bytes++) {
    const int value = c.client.read();
    if (value < 0) break;
    c.lastActivity = now;
    const char ch = static_cast<char>(value);
    if (c.sawCR) {
      if (ch != '\n') {
        fail(index, 400, Error::Transfer);
        return;
      }
      c.sawCR = false;
      c.line[c.lineSize] = 0;
      c.lineSize = 0;
      if (c.phase == Phase::Request) {
        char method[12];
        if (!localsendParseRequestLine(c.line, method, sizeof(method), c.target, sizeof(c.target))) {
          fail(index, 400, Error::Transfer);
          return;
        }
        c.route = matchLocalsendRoute(method, c.target);
        c.phase = Phase::Headers;
      } else {
        const HeaderFeed result = c.headers.feed(c.line);
        if (result == HeaderFeed::Overflow || c.headers.invalid) {
          fail(index, 400, Error::Transfer);
          return;
        }
        if (result == HeaderFeed::Done) {
          dispatch(index);
          return;
        }
      }
      continue;
    }
    if (ch == '\r') {
      c.sawCR = true;
    } else if (value == 0 || ch == '\n' || c.lineSize + 1 >= sizeof(c.line)) {
      fail(index, 400, Error::Transfer);
      return;
    } else {
      c.line[c.lineSize++] = ch;
    }
  }
}

void LocalsendServer::dispatch(size_t index) {
  auto& c = connections[index];
  if (c.headers.unsupportedExpectation) {
    reply(index, 417);
    return;
  }
  switch (c.route) {
    case LocalsendRoute::Unknown:
      reply(index, 404);
      return;
    case LocalsendRoute::Info: {
      const size_t size = service.buildInfo(c.response + BODY_OFFSET, sizeof(c.response) - BODY_OFFSET);
      reply(index, size ? 200 : 500, size);
      return;
    }
    case LocalsendRoute::Cancel: {
      char session[LocalsendService::SESSION_ID_LEN + 1];
      if (!queryParam(c.target, "sessionId", session, sizeof(session))) {
        reply(index, 400);
        return;
      }
      if (!service.cancel(session, c.peer)) {
        reply(index, 403);
        return;
      }
      for (size_t i = 0; i < CLIENT_COUNT; i++) {
        if (connections[i].entry) fail(i, 403, Error::Cancelled);
      }
      setError(Error::Cancelled);
      reply(index, 200);
      return;
    }
    case LocalsendRoute::Upload:
      beginUpload(index);
      if (!c.entry) return;
      break;
    case LocalsendRoute::PrepareUpload:
    case LocalsendRoute::Register:
      if (!c.headers.chunked && !c.headers.hasLength()) {
        reply(index, 411);
        return;
      }
      if (c.headers.contentLength > JSON_CAP) {
        reply(index, 413);
        return;
      }
      if (jsonOwner >= 0 || (c.route == LocalsendRoute::PrepareUpload && service.sessionActive(millis()))) {
        reply(index, 409);
        return;
      }
      jsonOwner = static_cast<int>(index);
      c.chunks = LocalsendChunkDecoder(JSON_CAP, false);
      break;
  }
  c.phase = Phase::Body;
  if (c.headers.expectContinue) {
    static constexpr char CONTINUE[] = "HTTP/1.1 100 Continue\r\n\r\n";
    memcpy(c.response, CONTINUE, sizeof(CONTINUE) - 1);
    c.responseSize = sizeof(CONTINUE) - 1;
    c.responseSent = 0;
    c.phase = Phase::Continue;
  }
}

void LocalsendServer::readBody(size_t index) {
  auto& c = connections[index];
  if (!c.headers.chunked && c.bodyBytes == c.headers.contentLength) {
    completeBody(index);
    return;
  }
  const int available = c.client.available();
  if (available <= 0) return;
  size_t want = std::min(sizeof(io), static_cast<size_t>(available));
  if (!c.headers.chunked) want = std::min<uint64_t>(want, c.headers.contentLength - c.bodyBytes);
  const int received = c.client.read(io, want);
  if (received <= 0) return;
  c.lastActivity = millis();
  size_t produced = static_cast<size_t>(received);
  bool complete = false;
  if (c.headers.chunked) {
    size_t used = 0;
    const auto result = c.chunks.feed(io, produced, &used, io, sizeof(io), &produced);
    if (result == ChunkFeed::Invalid || used != static_cast<size_t>(received)) {
      fail(index, 400, Error::Transfer);
      return;
    }
    complete = result == ChunkFeed::Complete;
  }
  if (c.entry) {
    if (c.file.write(io, produced) != produced ||
        (c.entry->hasChecksum && mbedtls_sha256_update(&c.checksum, io, produced) != 0)) {
      fail(index, 500, Error::FileStorage);
      return;
    }
    service.touch(millis());
  } else {
    if (c.bodyBytes + produced > JSON_CAP) {
      fail(index, 413, Error::Transfer);
      return;
    }
    memcpy(jsonBody + c.bodyBytes, io, produced);
  }
  c.bodyBytes += produced;
  if (!c.headers.chunked) complete = c.bodyBytes == c.headers.contentLength;
  if (complete) completeBody(index);
}

void LocalsendServer::completeBody(size_t index) {
  auto& c = connections[index];
  if (c.entry) {
    completeUpload(index);
    return;
  }
  jsonBody[c.bodyBytes] = 0;
  prepare(index);
  jsonOwner = -1;
}

void LocalsendServer::prepare(size_t index) {
  auto& c = connections[index];
  JsonArena arena(jsonArena, sizeof(jsonArena));
  JsonDocument document(&arena);
  JsonBodyReader reader(jsonBody, static_cast<size_t>(c.bodyBytes));
  const auto error = deserializeJson(document, reader, DeserializationOption::NestingLimit(8));
  if (error || !reader.finished() || !document.is<JsonObject>()) {
    LOG_ERR(TAG, "Invalid control JSON: %s", error.c_str());
    reply(index, error == DeserializationError::NoMemory ? 413 : 400);
    return;
  }
  if (c.route == LocalsendRoute::Register) {
    const size_t size = service.buildInfo(c.response + BODY_OFFSET, sizeof(c.response) - BODY_OFFSET);
    reply(index, size ? 200 : 500, size);
    return;
  }
  if (!document["files"].is<JsonObject>()) {
    reply(index, 400);
    return;
  }
  const JsonObject files = document["files"].as<JsonObject>();
  if (files.size() == 0) {
    reply(index, 204);
    return;
  }
  int count = 0;
  for (JsonPair pair : files) {
    if (count == LocalsendService::MAX_FILES) break;
    const JsonObjectConst file = pair.value().as<JsonObjectConst>();
    const JsonString key = pair.key();
    if (file.isNull() || !key.c_str() || strlen(key.c_str()) != key.size() ||
        !validJsonString(file["fileName"]) || !file["size"].is<uint64_t>()) continue;
    // The response map uses the sender's map key, never an inconsistent inner id.
    if (!file["id"].isNull() &&
        (!validJsonString(file["id"]) || strcmp(file["id"].as<const char*>(), key.c_str()) != 0)) continue;
    if (!file["sha256"].isNull() && !validJsonString(file["sha256"])) continue;
    incoming[count++] = {key.c_str(), file["fileName"].as<const char*>(), file["size"].as<uint64_t>(),
                         file["sha256"].as<const char*>()};
  }
  if (!count) {
    reply(index, 400);
    return;
  }
  const auto result = service.prepareUpload(incoming, count, millis(), c.peer);
  switch (result) {
    case LocalsendPrepareStatus::Busy:
      reply(index, 409);
      return;
    case LocalsendPrepareStatus::NoFiles:
      reply(index, 204);
      return;
    case LocalsendPrepareStatus::Invalid:
      reply(index, 400);
      return;
    case LocalsendPrepareStatus::Ok:
      break;
  }
  const size_t size = service.buildPrepareResponse(c.response + BODY_OFFSET, sizeof(c.response) - BODY_OFFSET);
  if (!size) {
    service.endSession();
    reply(index, 500);
    return;
  }
  status.error = Error::None;
  ++status.revision;
  reply(index, 200, size);
}

void LocalsendServer::beginUpload(size_t index) {
  auto& c = connections[index];
  char session[17], fileId[65], token[17];
  if (!queryParam(c.target, "sessionId", session, sizeof(session)) ||
      !queryParam(c.target, "fileId", fileId, sizeof(fileId)) || !queryParam(c.target, "token", token, sizeof(token))) {
    reply(index, 400);
    return;
  }
  const auto* entry = service.validateUpload(session, fileId, token, millis(), c.peer);
  if (!entry) {
    reply(index, 403);
    return;
  }
  size_t active = 0;
  for (const auto& other : connections) {
    if (other.entry == entry) {
      reply(index, 409);
      return;
    }
    if (other.entry) ++active;
  }
  if (active >= CLIENT_COUNT - 1) {
    reply(index, 409);
    return;
  }
  if (!c.headers.chunked && (!c.headers.hasLength() || c.headers.contentLength != entry->size)) {
    reply(index, 400);
    return;
  }
  c.entry = entry;
  c.chunks = LocalsendChunkDecoder(entry->size);
  if (!chooseDestination(c)) {
    fail(index, 500, Error::FileStorage);
    return;
  }
  snprintf(c.partialPath, sizeof(c.partialPath), "%s/.localsend-%s.part", RECEIVE_DIR, entry->token);
  c.file = Storage.open(c.partialPath, O_WRONLY | O_CREAT | O_EXCL);
  if (!c.file) {
    // An exclusive-create failure does not make an existing path ours to delete.
    c.partialPath[0] = 0;
    fail(index, 500, Error::FileStorage);
    return;
  }
  if (entry->hasChecksum) {
    mbedtls_sha256_init(&c.checksum);
    c.checksumStarted = true;
    if (mbedtls_sha256_starts(&c.checksum, 0) != 0) {
      fail(index, 500, Error::Transfer);
      return;
    }
  }
  status.error = Error::None;
  ++status.revision;
}

bool LocalsendServer::chooseDestination(Connection& c) {
  for (int suffix = 0; suffix <= 999; suffix++) {
    if (!LocalsendService::makeCollisionName(c.entry->fileName, c.finalName, sizeof(c.finalName), suffix)) return false;
    snprintf(c.finalPath, sizeof(c.finalPath), "%s/%s", RECEIVE_DIR, c.finalName);
    if (Storage.exists(c.finalPath)) continue;
    bool reserved = false;
    for (const auto& other : connections) {
      if (&other != &c && other.entry && strcasecmp(other.finalPath, c.finalPath) == 0) reserved = true;
    }
    if (!reserved) return true;
  }
  return false;
}

void LocalsendServer::completeUpload(size_t index) {
  auto& c = connections[index];
  if (c.bodyBytes != c.entry->size) {
    fail(index, 400, Error::Transfer);
    return;
  }
  if (c.entry->hasChecksum) {
    uint8_t checksum[32];
    if (mbedtls_sha256_finish(&c.checksum, checksum) != 0 || memcmp(checksum, c.entry->sha256, sizeof(checksum)) != 0) {
      fail(index, 422, Error::Transfer);
      return;
    }
  }
  // HalFile::close returns the SdFat sync/close result; flush() alone cannot
  // report failure. Never publish a filename before all bytes are durable.
  const bool sizeMatches = c.file.fileSize64() == c.entry->size;
  const bool closed = c.file.close();
  if (!sizeMatches || !closed || !chooseDestination(c) || !Storage.rename(c.partialPath, c.finalPath)) {
    fail(index, 500, Error::FileStorage);
    return;
  }
  c.partialPath[0] = 0;
  snprintf(status.fileName, sizeof(status.fileName), "%s", c.finalName);
  ++status.completedFiles;
  ++status.revision;
  service.markReceived(c.entry->id, millis());
  LOG_INF(TAG, "Received %s (%llu bytes)", c.finalPath, static_cast<unsigned long long>(c.bodyBytes));
  releaseUpload(c);
  reply(index, 200);
}

void LocalsendServer::releaseUpload(Connection& c) {
  if (c.file) c.file.close();
  c.file = HalFile();
  if (c.partialPath[0]) {
    if (!Storage.remove(c.partialPath)) setError(Error::FileStorage);
    c.partialPath[0] = 0;
  }
  if (c.checksumStarted) {
    mbedtls_sha256_free(&c.checksum);
    c.checksumStarted = false;
  }
  c.entry = nullptr;
}

void LocalsendServer::closeConnection(size_t index) {
  auto& c = connections[index];
  releaseUpload(c);
  c.client.stop();
  if (jsonOwner == static_cast<int>(index)) jsonOwner = -1;
  c.phase = Phase::Idle;
  c.route = LocalsendRoute::Unknown;
  c.headers = LocalsendHeaderParser();
  c.bodyBytes = 0;
  c.lineSize = 0;
  c.sawCR = false;
  c.responseSize = c.responseSent = 0;
  c.finalPath[0] = c.finalName[0] = 0;
}

void LocalsendServer::reply(size_t index, int code, size_t bodyLength) {
  auto& c = connections[index];
  char header[BODY_OFFSET];
  const int size = snprintf(header, sizeof(header),
                            "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\n"
                            "Connection: close\r\n\r\n",
                            code, reasonPhrase(code), static_cast<unsigned>(bodyLength));
  if (size <= 0 || static_cast<size_t>(size) >= sizeof(header) || bodyLength > sizeof(c.response) - BODY_OFFSET) {
    closeConnection(index);
    return;
  }
  memmove(c.response + size, c.response + BODY_OFFSET, bodyLength);
  memcpy(c.response, header, size);
  c.responseSize = size + bodyLength;
  c.responseSent = 0;
  c.lastActivity = millis();
  c.phase = Phase::Reply;
}

void LocalsendServer::fail(size_t index, int code, Error error) {
  setError(error);
  releaseUpload(connections[index]);
  if (jsonOwner == static_cast<int>(index)) jsonOwner = -1;
  reply(index, code);
}

void LocalsendServer::updateProgress() {
  status.received = 0;
  status.total = 0;
  status.receiving = false;
  for (const auto& c : connections) {
    if (!c.entry) continue;
    if (!status.receiving) snprintf(status.fileName, sizeof(status.fileName), "%s", c.finalName);
    status.receiving = true;
    status.received += c.bodyBytes;
    status.total += c.entry->size;
  }
}
