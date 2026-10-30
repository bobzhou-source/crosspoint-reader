// Copyright (c) 2026 Pavel Liashkov. MIT License; see ../LICENSE.
// CrossPoint transport adaptation of PapyriX's LocalSend receiver.
#pragma once

#include <HalStorage.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <mbedtls/sha256.h>

#include "LocalsendHttp.h"
#include "LocalsendService.h"

// Single-task, bounded polling. Two uploads may run concurrently; a third
// connection remains available for discovery and cancellation. No worker task.
class LocalsendServer {
 public:
  static constexpr uint16_t PORT = 53317;
  static constexpr const char* RECEIVE_DIR = "/received";
  enum class Error : uint8_t { None, Network, FileStorage, Transfer, Cancelled };
  struct Status {
    uint64_t received = 0;
    uint64_t total = 0;
    uint32_t completedFiles = 0;
    uint32_t revision = 0;
    bool receiving = false;
    Error error = Error::None;
    char fileName[160] = {};
  };

  LocalsendServer() = default;
  ~LocalsendServer() { stop(); }
  bool begin();
  void poll();
  void stop();
  bool isRunning() const { return running; }
  const Status& getStatus() const { return status; }

 private:
  static constexpr size_t JSON_CAP = 16384;
  static constexpr size_t IO_CAP = 4096;
  static constexpr size_t RESPONSE_CAP = 3072;
  static constexpr size_t CLIENT_COUNT = 3;
  static constexpr uint32_t STALL_MS = 15000;
  static constexpr uint32_t CONTROL_TIMEOUT_MS = 30000;
  enum class Phase : uint8_t { Idle, Request, Headers, Body, Reply, Continue };
  struct Connection {
    WiFiClient client;
    Phase phase = Phase::Idle;
    LocalsendRoute route = LocalsendRoute::Unknown;
    LocalsendHeaderParser headers;
    LocalsendChunkDecoder chunks{0};
    uint32_t startedAt = 0;
    uint32_t lastActivity = 0;
    uint64_t bodyBytes = 0;
    size_t lineSize = 0;
    size_t responseSize = 0;
    size_t responseSent = 0;
    bool sawCR = false;
    bool checksumStarted = false;
    char peer[46] = {};
    char line[512] = {};
    char target[MAX_TARGET_LEN + 1] = {};
    char response[RESPONSE_CAP] = {};
    char finalName[160] = {};
    char finalPath[192] = {};
    char partialPath[80] = {};
    const LocalsendFileEntry* entry = nullptr;
    HalFile file;
    mbedtls_sha256_context checksum{};
  };

  WiFiServer listener{PORT};
  WiFiUDP discovery;
  LocalsendService service;
  Connection connections[CLIENT_COUNT];
  Status status;
  bool running = false;
  int jsonOwner = -1;
  uint32_t lastAnnounce = 0;
  uint32_t lastDiscoveryReply = 0;
  unsigned announcesLeft = 0;
  char fingerprint[33] = {};
  uint8_t io[IO_CAP] = {};
  char jsonBody[JSON_CAP + 1] = {};
  // Bounded ArduinoJson arena: parser allocations cannot consume the Wi-Fi heap.
  alignas(std::max_align_t) uint8_t jsonArena[12288] = {};
  LocalsendIncomingFile incoming[LocalsendService::MAX_FILES] = {};

  void announce(bool initial);
  void pollDiscovery();
  void pollConnection(size_t index);
  void dispatch(size_t index);
  void readBody(size_t index);
  void completeBody(size_t index);
  void prepare(size_t index);
  void beginUpload(size_t index);
  void completeUpload(size_t index);
  bool chooseDestination(Connection& connection);
  bool removeStalePartials();
  void releaseUpload(Connection& connection);
  void closeConnection(size_t index);
  void reply(size_t index, int code, size_t bodyLength = 0);
  void fail(size_t index, int code, Error error);
  void setError(Error error);
  void updateProgress();
};
