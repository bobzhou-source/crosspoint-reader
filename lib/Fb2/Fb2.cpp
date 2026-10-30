#include "Fb2.h"

#include <ConvertedEpub.h>
#include <Logging.h>
#include <Memory.h>
#include <expat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Fb2Encoding.h"

namespace {
constexpr uint32_t FORMAT_VERSION = 1;
constexpr size_t XML_MEMORY_LIMIT = 64 * 1024;
struct XmlBudget { size_t used = 0; };
struct alignas(std::max_align_t) Allocation { size_t size; XmlBudget* owner; };
std::atomic<XmlBudget*> activeBudget{nullptr};

// Expat owns/frees these allocations through its C allocator suite. The budget
// includes Expat's interned names and unfinished tokens, not just input chunks.
void* xmlAllocate(size_t size) {
  XmlBudget* budget = activeBudget.load();
  if (!budget || size > XML_MEMORY_LIMIT - budget->used || sizeof(Allocation) > XML_MEMORY_LIMIT - budget->used - size)
    return nullptr;
  auto* block = static_cast<Allocation*>(malloc(sizeof(Allocation) + size));
  if (!block) return nullptr;
  block->size = size;
  block->owner = budget;
  budget->used += sizeof(Allocation) + size;
  return block + 1;
}
void xmlFree(void* memory) {
  if (!memory) return;
  auto* block = static_cast<Allocation*>(memory) - 1;
  block->owner->used -= sizeof(Allocation) + block->size;
  free(block);
}
void* xmlReallocate(void* memory, size_t size) {
  if (!memory) return xmlAllocate(size);
  if (!size) { xmlFree(memory); return nullptr; }
  auto* block = static_cast<Allocation*>(memory) - 1;
  if (size > XML_MEMORY_LIMIT - block->owner->used + block->size) return nullptr;
  const size_t oldSize = block->size;
  auto* resized = static_cast<Allocation*>(realloc(block, sizeof(Allocation) + size));
  if (!resized) return nullptr;
  resized->owner->used = resized->owner->used - oldSize + size;
  resized->size = size;
  return resized + 1;
}

const char* localName(const char* name) {
  const char* colon = strrchr(name, ':');
  return colon ? colon + 1 : name;
}
const char* attribute(const char** attrs, const char* name) {
  for (size_t i = 0; attrs && attrs[i]; i += 2) if (!strcmp(localName(attrs[i]), name)) return attrs[i + 1];
  return "";
}
uint64_t imageHash(const char* id) {
  uint64_t hash = 14695981039346656037ULL;
  while (*id) hash = (hash ^ static_cast<uint8_t>(*id++)) * 1099511628211ULL;
  return hash;
}
void imageName(char* out, size_t size, uint64_t hash, uint8_t type) {
  snprintf(out, size, "images/%08lx%08lx.%s", static_cast<unsigned long>(hash >> 32),
           static_cast<unsigned long>(hash & 0xffffffff), type == 1 ? "png" : "jpg");
}
void append(char* buffer, size_t capacity, const char* data, size_t length) {
  const size_t used = strlen(buffer);
  if (length > capacity - 1 - used) {
    length = capacity - 1 - used;
    while (length && (static_cast<uint8_t>(data[length]) & 0xc0) == 0x80) --length;
  }
  if (length) memcpy(buffer + used, data, length);
  buffer[used + length] = 0;
}

class Fb2Conversion {
 public:
  explicit Fb2Conversion(ConvertedEpub& book) : book(book), imageIndexPath(book.path() + ".images.tmp") {}
  ~Fb2Conversion() {
    if (parser) XML_ParserFree(parser);
    imageIndex.close();
    Storage.remove(imageIndexPath.c_str());
  }
  bool run(const std::string& source) {
    XmlBudget* expected = nullptr;
    if (!activeBudget.compare_exchange_strong(expected, &budget)) {
      LOG_ERR("FB2", "Another FB2 conversion owns the parser budget");
      return false;
    }
    struct Release { ~Release() { activeBudget.store(nullptr); } } release;
    if (!Storage.openFileForWrite("FB2", imageIndexPath, imageIndex) || !parse(source, false)) return false;
    imageIndex.close();
    if (!Storage.openFileForRead("FB2", imageIndexPath, imageIndex)) return false;
    if (*title) book.setTitle(title);
    if (*author) book.setAuthor(author);
    if (*language) book.setLanguage(language);
    return parse(source, true) && hasBody;
  }

 private:
  ConvertedEpub& book;
  XmlBudget budget;
  std::string imageIndexPath;
  HalFile imageIndex;
  XML_Parser parser = nullptr;
  struct Frame { const char* output = nullptr; uint8_t capture = 0; } frames[64];
  uint8_t depth = 0;
  uint8_t bodyDepth = 0;
  uint8_t titleInfoDepth = 0;
  uint8_t authorDepth = 0;
  uint8_t headingDepth = 0;
  uint8_t binaryDepth = 0;
  bool rendering = false;
  bool ok = true;
  bool hasBody = false;
  bool binaryResource = false;
  bool base64Finished = false;
  uint8_t quartet[4] = {};
  uint8_t quartetSize = 0;
  uint32_t headingCount = 0;
  char title[512] = {};
  char author[256] = {};
  char language[48] = {};
  char heading[256] = {};
  char headingAnchor[40] = {};
  char input[1024];

  void stop(const char* reason) {
    LOG_ERR("FB2", "%s", reason);
    ok = false;
    XML_StopParser(parser, XML_FALSE);
  }
  bool raw(const char* text) {
    const size_t size = strlen(text);
    if (book.content().write(reinterpret_cast<const uint8_t*>(text), size) != size) {
      stop("XHTML write failed");
      return false;
    }
    return true;
  }
  bool text(const char* data, size_t size) {
    if (!ConvertedEpub::writeXml(book.content(), std::string_view(data, size))) {
      stop("XHTML text write failed");
      return false;
    }
    return true;
  }
  void attr(const char* name, const char* value) {
    if (!*value) return;
    raw(" "); raw(name); raw("=\""); text(value, strlen(value)); raw("\"");
  }
  bool parse(const std::string& source, bool render) {
    rendering = render;
    depth = bodyDepth = titleInfoDepth = authorDepth = headingDepth = binaryDepth = 0;
    hasBody = false;
    const XML_Memory_Handling_Suite memory = {xmlAllocate, xmlReallocate, xmlFree};
    parser = XML_ParserCreate_MM(nullptr, &memory, nullptr);
    if (!parser) { LOG_ERR("FB2", "Cannot allocate XML parser"); return false; }
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, startCallback, endCallback);
    XML_SetCharacterDataHandler(parser, textCallback);
    XML_SetUnknownEncodingHandler(parser, fb2Encoding, nullptr);
    XML_SetStartDoctypeDeclHandler(parser, doctypeCallback);
    XML_SetParamEntityParsing(parser, XML_PARAM_ENTITY_PARSING_NEVER);
    HalFile file;
    bool success = Storage.openFileForRead("FB2", source, file);
    uint32_t remaining = success ? file.size() : 0;
    uint32_t chunks = 0;
    while (success && remaining) {
      const int count = file.read(input, std::min<uint32_t>(sizeof(input), remaining));
      if (count <= 0) { success = false; break; }
      remaining -= count;
      success = XML_Parse(parser, input, count, remaining == 0) == XML_STATUS_OK && ok;
      if (++chunks % 16 == 0) vTaskDelay(1);
    }
    success = success && hasBody && depth == 0 && book.good();
    if (!success) {
      const char* detail = XML_ErrorString(XML_GetErrorCode(parser));
      LOG_ERR("FB2", "XML conversion failed: %s", detail ? detail : "missing body or I/O failure");
    }
    XML_ParserFree(parser);
    parser = nullptr;
    return success;
  }
  static void XMLCALL startCallback(void* context, const char* tag, const char** attrs) {
    static_cast<Fb2Conversion*>(context)->start(localName(tag), attrs);
  }
  static void XMLCALL endCallback(void* context, const char* tag) {
    static_cast<Fb2Conversion*>(context)->end(localName(tag));
  }
  static void XMLCALL textCallback(void* context, const char* data, int length) {
    static_cast<Fb2Conversion*>(context)->characters(data, length);
  }
  static void XMLCALL doctypeCallback(void* context, const char*, const char*, const char*, int) {
    static_cast<Fb2Conversion*>(context)->stop("DTD declarations are not supported");
  }

  void start(const char* tag, const char** attrs) {
    if (!ok) return;
    if (depth == 64) { stop("FB2 nesting exceeds 64 elements"); return; }
    if (!depth && strcmp(tag, "FictionBook")) { stop("Not a FictionBook document"); return; }
    Frame& frame = frames[depth++];
    frame = {};
    if (binaryDepth) return;
    if (!strcmp(tag, "binary")) {
      binaryDepth = depth;
      if (!rendering) startBinary(attrs);
      return;
    }
    if (!strcmp(tag, "title-info")) titleInfoDepth = depth;
    if (titleInfoDepth && !rendering) {
      if (!strcmp(tag, "book-title")) frame.capture = 1;
      else if (!strcmp(tag, "lang")) frame.capture = 3;
      else if (!strcmp(tag, "author")) {
        authorDepth = depth;
        if (*author) append(author, sizeof(author), "; ", 2);
      } else if (authorDepth && (!strcmp(tag, "first-name") || !strcmp(tag, "middle-name") ||
                                !strcmp(tag, "last-name") || !strcmp(tag, "nickname"))) {
        frame.capture = 2;
        const size_t size = strlen(author);
        if (size && author[size - 1] != ' ') append(author, sizeof(author), " ", 1);
      }
    }
    if (!strcmp(tag, "body")) { bodyDepth = depth; hasBody = true; }
    if (!rendering || !bodyDepth) return;
    const char* mapped = nullptr;
    if (!strcmp(tag, "body") || !strcmp(tag, "section") || !strcmp(tag, "stanza") || !strcmp(tag, "poem")) mapped = "div";
    else if (!strcmp(tag, "title")) {
      headingDepth = depth;
      heading[0] = 0;
      snprintf(headingAnchor, sizeof(headingAnchor), "fb2-title-%lu", static_cast<unsigned long>(++headingCount));
      mapped = "div";
    } else if (!strcmp(tag, "p")) mapped = headingDepth ? "h2" : "p";
    else if (!strcmp(tag, "subtitle")) mapped = "h3";
    else if (!strcmp(tag, "v") || !strcmp(tag, "text-author")) mapped = "p";
    else if (!strcmp(tag, "cite") || !strcmp(tag, "epigraph") || !strcmp(tag, "annotation")) mapped = "blockquote";
    else if (!strcmp(tag, "strong")) mapped = "strong";
    else if (!strcmp(tag, "emphasis")) mapped = "em";
    else if (!strcmp(tag, "strikethrough")) mapped = "s";
    else if (!strcmp(tag, "sub") || !strcmp(tag, "sup") || !strcmp(tag, "code") || !strcmp(tag, "table") ||
             !strcmp(tag, "tr") || !strcmp(tag, "td") || !strcmp(tag, "th")) {
      // These names need stable storage after the callback's Expat buffer expires.
      static constexpr const char* NAMES[] = {"sub", "sup", "code", "table", "tr", "td", "th"};
      for (const char* name : NAMES) if (!strcmp(name, tag)) mapped = name;
    } else if (!strcmp(tag, "a")) mapped = "a";
    else if (!strcmp(tag, "empty-line")) { raw("<p><br/></p>"); return; }
    else if (!strcmp(tag, "image")) { emitImage(attrs); return; }
    if (!mapped) return;
    frame.output = mapped;
    raw("<"); raw(mapped);
    if (headingDepth == depth) attr("id", headingAnchor);
    else attr("id", attribute(attrs, "id"));
    if (!strcmp(mapped, "a")) {
      attr("href", attribute(attrs, "href"));
      if (!strcmp(attribute(attrs, "type"), "note")) attr("epub:type", "noteref");
    }
    if (!strcmp(mapped, "td") || !strcmp(mapped, "th")) {
      attr("colspan", attribute(attrs, "colspan"));
      attr("rowspan", attribute(attrs, "rowspan"));
    }
    raw(">");
  }

  void end(const char* tag) {
    if (!ok || !depth) return;
    if (binaryDepth == depth) {
      if (binaryResource && (quartetSize || !book.endResource())) { stop("Invalid base64 image"); return; }
      binaryDepth = 0;
      binaryResource = false;
    }
    const Frame& frame = frames[depth - 1];
    if (rendering && frame.output) { raw("</"); raw(frame.output); raw(">"); }
    if (headingDepth == depth) {
      if (*heading && !book.addTocEntry(heading, headingAnchor)) stop("Cannot write contents entry");
      headingDepth = 0;
    }
    if (titleInfoDepth == depth) titleInfoDepth = 0;
    if (authorDepth == depth) authorDepth = 0;
    if (bodyDepth == depth) bodyDepth = 0;
    if (headingDepth && !strcmp(tag, "p")) append(heading, sizeof(heading), " ", 1);
    --depth;
  }

  void characters(const char* data, size_t size) {
    if (!ok) return;
    if (binaryDepth) {
      if (binaryResource) decodeBase64(data, size);
      return;
    }
    if (!rendering) {
      for (int i = depth - 1; i >= 0; --i) {
        if (frames[i].capture == 1) { append(title, sizeof(title), data, size); break; }
        if (frames[i].capture == 2) { append(author, sizeof(author), data, size); break; }
        if (frames[i].capture == 3) { append(language, sizeof(language), data, size); break; }
      }
    } else if (bodyDepth) {
      text(data, size);
      if (headingDepth) append(heading, sizeof(heading), data, size);
    }
  }

  void startBinary(const char** attrs) {
    const char* type = attribute(attrs, "content-type");
    const uint8_t kind = !strcmp(type, "image/png") ? 1 : (!strcmp(type, "image/jpeg") || !strcmp(type, "image/jpg")) ? 2 : 0;
    const char* id = attribute(attrs, "id");
    if (!kind || !*id) return;
    const uint64_t hash = imageHash(id);
    char name[64];
    imageName(name, sizeof(name), hash, kind);
    if (!book.beginResource(name, kind == 1 ? "image/png" : "image/jpeg") ||
        imageIndex.write(&hash, sizeof(hash)) != sizeof(hash) || imageIndex.write(&kind, 1) != 1) {
      stop("Cannot store FB2 image"); return;
    }
    binaryResource = true;
    base64Finished = false;
    quartetSize = 0;
  }

  void decodeBase64(const char* text, size_t size) {
    for (size_t i = 0; i < size && ok; ++i) {
      const char c = text[i];
      if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
      if (base64Finished) { stop("Trailing base64 data"); return; }
      const int value = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 :
                        c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : c == '=' ? 64 : -1;
      if (value < 0) { stop("Invalid base64 byte"); return; }
      quartet[quartetSize++] = value;
      if (quartetSize != 4) continue;
      if (quartet[0] == 64 || quartet[1] == 64 || (quartet[2] == 64 && quartet[3] != 64) ||
          (quartet[2] == 64 && (quartet[1] & 15)) ||
          (quartet[3] == 64 && quartet[2] != 64 && (quartet[2] & 3))) {
        stop("Invalid base64 padding"); return;
      }
      const uint8_t bytes[] = {static_cast<uint8_t>((quartet[0] << 2) | (quartet[1] >> 4)),
                               static_cast<uint8_t>((quartet[1] << 4) | (quartet[2] >> 2)),
                               static_cast<uint8_t>((quartet[2] << 6) | quartet[3])};
      const size_t count = quartet[2] == 64 ? 1 : quartet[3] == 64 ? 2 : 3;
      if (book.resource().write(bytes, count) != count) { stop("Image write failed"); return; }
      base64Finished = count != 3;
      quartetSize = 0;
    }
  }

  void emitImage(const char** attrs) {
    const char* href = attribute(attrs, "href");
    uint8_t kind = 0;
    if (*href == '#' && imageIndex.seek(0)) {
      const uint64_t target = imageHash(href + 1);
      uint64_t hash;
      while (imageIndex.read(&hash, sizeof(hash)) == sizeof(hash)) {
        uint8_t candidate;
        if (imageIndex.read(&candidate, 1) != 1) { stop("Image index read failed"); return; }
        if (hash == target) { kind = candidate; break; }
      }
    }
    if (!kind) {
      const char* alt = attribute(attrs, "alt");
      text(alt, strlen(alt));
      return;
    }
    char name[64];
    imageName(name, sizeof(name), imageHash(href + 1), kind);
    raw("<img"); attr("src", name); attr("alt", attribute(attrs, "alt")); raw("/>");
  }
};
}  // namespace

std::string Fb2::prepare(const std::string& sourcePath, const std::string& cacheRoot) {
  // Builder (~6 KiB) and SAX state (~3 KiB) exceed the task stack budget and are
  // allocated once; neither grows with file size. Expat is capped at 64 KiB.
  auto book = makeUniqueNoThrow<ConvertedEpub>(sourcePath, cacheRoot, "fb2", FORMAT_VERSION);
  if (!book) { LOG_ERR("FB2", "No memory for converter"); return {}; }
  const auto state = book->prepare();
  if (state == ConvertedEpub::Preparation::Cached) return book->path();
  if (state == ConvertedEpub::Preparation::Failed || !book->begin()) return {};
  auto parser = makeUniqueNoThrow<Fb2Conversion>(*book);
  if (!parser) { LOG_ERR("FB2", "No memory for SAX state"); return {}; }
  if (!parser->run(sourcePath)) return {};
  parser.reset();
  if (!book->finish()) return {};
  return book->path();
}
