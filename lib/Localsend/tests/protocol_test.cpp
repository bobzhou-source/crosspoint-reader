// MIT License; see ../LICENSE.
// Host-only regression cases. No Arduino, Wi-Fi, SD card or physical reader.
#include "LocalsendHttp.h"
#include "LocalsendService.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
uint32_t randomWord() {
  static uint32_t value = 0x12345678;
  value = value * 1664525u + 1013904223u;
  return value;
}

struct Session {
  LocalsendService service;
  char id[17] = {};
  char token[17] = {};
  Session() { service.begin("CrossPoint", "aabbcc", 53317, randomWord); }
  void prepare(const LocalsendIncomingFile* files, int count, uint32_t now = 100) {
    assert(service.prepareUpload(files, count, now, "192.168.0.5") == LocalsendPrepareStatus::Ok);
    char response[3072];
    assert(service.buildPrepareResponse(response, sizeof(response)));
    assert(sscanf(response, "{\"sessionId\":\"%16[^\"]\"", id) == 1);
    const char* first = strstr(response, "\"files\":{");
    assert(first);
    first = strstr(first + 9, "\":\"");
    assert(first);
    assert(sscanf(first + 3, "%16[^\"]", token) == 1);
  }
};

ChunkFeed decode(const std::string& wire, uint64_t expected, size_t slice, std::string& output, bool exact = true) {
  LocalsendChunkDecoder decoder(expected, exact);
  size_t offset = 0;
  ChunkFeed result = ChunkFeed::NeedInput;
  while (offset < wire.size()) {
    const size_t count = std::min(slice, wire.size() - offset);
    size_t consumed = 0;
    while (consumed < count) {
      uint8_t buffer[3];  // Deliberately smaller than a data chunk.
      size_t used = 0, produced = 0;
      result = decoder.feed(reinterpret_cast<const uint8_t*>(wire.data() + offset + consumed), count - consumed,
                            &used, buffer, sizeof(buffer), &produced);
      output.append(reinterpret_cast<const char*>(buffer), produced);
      consumed += used;
      if (result != ChunkFeed::NeedInput) return result;
      assert(used || produced);
    }
    offset += count;
  }
  return result;
}

void testSessions() {
  const LocalsendIncomingFile files[] = {{"first", "../../book.epub", 7}, {"second", "notes.md", 0}};
  Session session;
  session.prepare(files, 2);
  assert(session.service.sessionActive(100));
  assert(session.service.prepareUpload(files, 2, 101, "192.168.0.5") == LocalsendPrepareStatus::Busy);
  assert(!session.service.validateUpload(session.id, "first", session.token, 101, "192.168.0.6"));
  assert(!session.service.validateUpload(session.id, "first", "wrong", 101, "192.168.0.5"));
  assert(!session.service.validateUpload("wrong", "first", session.token, 101, "192.168.0.5"));
  const auto* entry = session.service.validateUpload(session.id, "first", session.token, 101, "192.168.0.5");
  assert(entry && strcmp(entry->fileName, "book.epub") == 0 && entry->size == 7);
  session.service.markReceived("first", 102);
  assert(!session.service.validateUpload(session.id, "first", session.token, 103, "192.168.0.5"));
  assert(session.service.sessionActive(103));
  assert(!session.service.cancel(session.id, "192.168.0.6"));
  assert(session.service.cancel(session.id, "192.168.0.5"));
  assert(!session.service.sessionActive(103));
  session.prepare(files, 1, 200);
  session.service.markReceived("first", 201);
  assert(!session.service.sessionActive(201));
  session.prepare(files, 1, 300);
  assert(session.service.sessionActive(60300));
  assert(!session.service.sessionActive(60301));
  session.prepare(files, 1, UINT32_MAX - 100);
  session.service.touch(10);
  assert(session.service.sessionActive(60010));
  assert(!session.service.sessionActive(60011));

  LocalsendIncomingFile invalid[] = {{"a", "..", 1}, {"b", "book.epub", LocalsendService::MAX_FILE_BYTES + 1},
                                     {"c\n", "book.epub", 1}, {"d", "book.epub", 1, "not a hash"}};
  assert(session.service.prepareUpload(invalid, 4, 70000, "192.168.0.5") == LocalsendPrepareStatus::Invalid);
  assert(session.service.prepareUpload(nullptr, 0, 70000, "192.168.0.5") == LocalsendPrepareStatus::NoFiles);
  assert(session.service.prepareUpload(files, 1, 70000, "") == LocalsendPrepareStatus::Invalid);

  const char* hash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  const LocalsendIncomingFile hashed[] = {{"empty", "zero.txt", 0, hash}};
  session.prepare(hashed, 1, 70001);
  entry = session.service.validateUpload(session.id, "empty", session.token, 70002, "192.168.0.5");
  assert(entry && entry->hasChecksum && entry->sha256[0] == 0xe3 && entry->sha256[31] == 0x55);
  uint8_t decoded[32];
  assert(LocalsendService::parseSha256(hash, decoded));
  assert(!LocalsendService::parseSha256("", decoded));
  assert(!LocalsendService::parseSha256("x3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", decoded));

  Session escaped;
  const LocalsendIncomingFile odd[] = {{"a\"b\\c", "book.txt", 1}, {"a\"b\\c", "duplicate.txt", 1}};
  assert(escaped.service.prepareUpload(odd, 2, 0, "192.168.0.5") == LocalsendPrepareStatus::Ok);
  char response[3072];
  assert(escaped.service.buildPrepareResponse(response, sizeof(response)));
  assert(strstr(response, "a\\\"b\\\\c"));
  assert(!escaped.service.buildPrepareResponse(response, 8));
  assert(escaped.service.buildAnnounce(response, sizeof(response), true));
  assert(strstr(response, "\"protocol\":\"http\"") && strstr(response, "\"port\":53317"));
  assert(strstr(response, "\"deviceModel\":\"CrossPoint\""));
  assert(escaped.service.buildInfo(response, sizeof(response)) && !strstr(response, "announce"));
}

void testNames() {
  char name[160];
  assert(LocalsendService::sanitizeFileName("../../book.epub", name, sizeof(name)) && strcmp(name, "book.epub") == 0);
  assert(LocalsendService::sanitizeFileName("C:\\evil\\notes.txt", name, sizeof(name)) && strcmp(name, "notes.txt") == 0);
  assert(!LocalsendService::sanitizeFileName("..\\..", name, sizeof(name)));
  assert(!LocalsendService::sanitizeFileName("? . . ", name, sizeof(name)));
  assert(LocalsendService::sanitizeFileName("? ..foo?.txt. ", name, sizeof(name)) && strcmp(name, "foo.txt") == 0);
  assert(LocalsendService::sanitizeFileName("a\tb\n.txt", name, sizeof(name)) && strcmp(name, "ab.txt") == 0);
  const std::string longName = std::string(160, 'x') + ".epub";
  assert(LocalsendService::sanitizeFileName(longName.c_str(), name, sizeof(name)) && strlen(name) == 128);
  assert(strcmp(name + 123, ".epub") == 0);
  const std::string unicode = std::string(126, 'x') + "中文.epub";
  assert(LocalsendService::sanitizeFileName(unicode.c_str(), name, sizeof(name)));
  assert(strlen(name) <= 128 && strcmp(name + strlen(name) - 5, ".epub") == 0);
  assert(!LocalsendService::sanitizeFileName("\xC0\xAF" "escape.txt", name, sizeof(name)));
  assert(!LocalsendService::sanitizeFileName("\xED\xA0\x80" ".txt", name, sizeof(name)));
  assert(!LocalsendService::sanitizeFileName("\xE4\xB8", name, sizeof(name)));
  assert(LocalsendService::makeCollisionName("book.epub", name, sizeof(name), 2) && strcmp(name, "book (2).epub") == 0);
  assert(!LocalsendService::makeCollisionName("book.epub", name, 3, 1));
  assert(!LocalsendService::makeCollisionName("book.epub", name, sizeof(name), -1));
}

void testHttp() {
  char method[8], target[MAX_TARGET_LEN + 1], value[65];
  assert(localsendParseRequestLine("POST /api/localsend/v2/upload?fileId=x HTTP/1.1", method, sizeof(method), target,
                                   sizeof(target)));
  assert(matchLocalsendRoute(method, target) == LocalsendRoute::Upload);
  assert(matchLocalsendRoute("POST", "/api/localsend/v2/prepare-upload?pin=123") == LocalsendRoute::PrepareUpload);
  assert(matchLocalsendRoute("POST", "/api/localsend/v2/upload-extra?x") == LocalsendRoute::Unknown);
  assert(matchLocalsendRoute("GET", "/api/localsend/v2/prepare-upload") == LocalsendRoute::Unknown);
  assert(!localsendParseRequestLine("POST /x garbage", method, sizeof(method), target, sizeof(target)));
  assert(!localsendParseRequestLine("POST /x HTTP/1.1 junk", method, sizeof(method), target, sizeof(target)));
  assert(queryParam("/upload?fileId=a+b%2Fc%26d", "fileId", value, sizeof(value)) && strcmp(value, "a b/c&d") == 0);
  assert(!queryParam("/upload?fileId=%00", "fileId", value, sizeof(value)));
  assert(!queryParam("/upload?fileId=%xz", "fileId", value, sizeof(value)));
  assert(!queryParam("/upload?fileId=%", "fileId", value, sizeof(value)));

  LocalsendHeaderParser headers;
  assert(headers.feed("content-length: 7") == HeaderFeed::More && headers.contentLength == 7 && headers.hasLength());
  headers.feed("Transfer-Encoding: chunked");
  assert(headers.invalid);
  headers = LocalsendHeaderParser();
  headers.feed("Transfer-Encoding: chunked");
  headers.feed("Content-Length: 7");
  assert(headers.invalid);
  headers = LocalsendHeaderParser();
  headers.feed("Content-Length: 7");
  headers.feed("Content-Length: 7");
  assert(headers.invalid);
  for (const char* line : {"Content-Length: -1", "Content-Length: 18446744073709551616", "Transfer-Encoding: gzip",
                           "bad header", "Content-Length : 10"}) {
    headers = LocalsendHeaderParser();
    headers.feed(line);
    assert(headers.invalid);
  }
  headers = LocalsendHeaderParser();
  headers.feed("Expect: 100-continue");
  assert(headers.expectContinue && !headers.unsupportedExpectation);
  headers.feed("Expect: something-else");
  assert(headers.unsupportedExpectation);
  headers = LocalsendHeaderParser();
  for (int i = 0; i < LocalsendHeaderParser::MAX_LINES; i++) assert(headers.feed("X-A: b") == HeaderFeed::More);
  assert(headers.feed("X-A: b") == HeaderFeed::Overflow);
}

void testChunking() {
  const std::string wire = "4;ext=\"value\"\r\nWiki\r\n5\r\npedia\r\n0\r\nX-Test: yes\r\n\r\n";
  for (size_t split = 1; split <= wire.size(); split++) {
    std::string output;
    assert(decode(wire, 9, split, output) == ChunkFeed::Complete && output == "Wikipedia");
  }
  for (const std::string wire : {"A\r\n0123456789\r\n0\r\n\r\n", "z\r\n", "0\n\n", "1\r\nxX\n0\r\n\r\n",
                                 "0;\r\n\r\n", "0\r\nbad trailer\r\n\r\n", "fffffffffffffffff\r\n"}) {
    std::string output;
    assert(decode(wire, 9, 1, output) == ChunkFeed::Invalid);
  }
  std::string output;
  assert(decode("0\r\n\r\n", 0, 1, output) == ChunkFeed::Complete && output.empty());
  assert(decode("0\r\n\r\n", 1, 1, output) == ChunkFeed::Invalid);
  assert(decode("1\r\nx\r\n0\r\n\r\n", 100, 1, output, false) == ChunkFeed::Complete);
  output.clear();
  assert(decode("4\r\nabc", 4, 1, output) == ChunkFeed::NeedInput);
  uint8_t inPlace[] = "3\r\nabc\r\n0\r\n\r\n";
  LocalsendChunkDecoder decoder(3);
  size_t used = 0, produced = 0;
  assert(decoder.feed(inPlace, sizeof(inPlace) - 1, &used, inPlace, sizeof(inPlace), &produced) == ChunkFeed::Complete);
  assert(produced == 3 && memcmp(inPlace, "abc", 3) == 0);
}
}  // namespace

int main() {
  testSessions();
  testNames();
  testHttp();
  testChunking();
  puts("LocalSend portable protocol cases passed");
}
