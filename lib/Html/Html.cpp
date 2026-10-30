#include "Html.h"

#include <ConvertedEpub.h>
#include <Epub/htmlEntities.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {
constexpr uint32_t FORMAT_VERSION = 1;
constexpr size_t MAX_TAG = 2048;
constexpr size_t MAX_PATH = 1024;
constexpr size_t MAX_DEPTH = 48;
constexpr unsigned MAX_IMAGES = 128;
constexpr unsigned MAX_HEADINGS = 256;

bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f'; }
bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
char lower(char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
bool equal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}
int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = lower(c);
  return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
uint32_t xmlScalar(uint32_t c) {
  return c == 9 || c == 10 || c == 13 || (c >= 0x20 && c <= 0xd7ff) || (c >= 0xe000 && c <= 0xfffd) ||
                 (c >= 0x10000 && c <= 0x10ffff)
             ? c
             : 0xfffd;
}
size_t encode(uint32_t c, char* out) {
  c = xmlScalar(c);
  if (c < 0x80) {
    out[0] = static_cast<char>(c);
    return 1;
  }
  if (c < 0x800) {
    out[0] = static_cast<char>(0xc0 | (c >> 6));
    out[1] = static_cast<char>(0x80 | (c & 63));
    return 2;
  }
  if (c < 0x10000) {
    out[0] = static_cast<char>(0xe0 | (c >> 12));
    out[1] = static_cast<char>(0x80 | ((c >> 6) & 63));
    out[2] = static_cast<char>(0x80 | (c & 63));
    return 3;
  }
  out[0] = static_cast<char>(0xf0 | (c >> 18));
  out[1] = static_cast<char>(0x80 | ((c >> 12) & 63));
  out[2] = static_cast<char>(0x80 | ((c >> 6) & 63));
  out[3] = static_cast<char>(0x80 | (c & 63));
  return 4;
}
void appendScalar(char* out, size_t capacity, size_t& length, uint32_t c, bool normalizeSpace) {
  if (normalizeSpace && (c == 9 || c == 10 || c == 13 || c == 32 || c == 0xa0)) {
    if (length == 0 || out[length - 1] == ' ') return;
    c = ' ';
  }
  char encoded[4];
  const size_t count = encode(c, encoded);
  if (length + count >= capacity) return;
  memcpy(out + length, encoded, count);
  length += count;
  out[length] = '\0';
}

// The entity table is shared with the native EPUB reader. Unknown entities stay literal.
uint32_t numericEntity(std::string_view entity) {
  if (entity.size() < 4 || entity[0] != '&' || entity[1] != '#' || entity.back() != ';') return 0;
  size_t i = 2;
  const bool hexadecimal = lower(entity[i]) == 'x';
  if (hexadecimal) ++i;
  if (i == entity.size() - 1) return 0;
  uint32_t value = 0;
  const uint32_t base = hexadecimal ? 16 : 10;
  for (; i + 1 < entity.size(); ++i) {
    const int digit = hexadecimal ? hex(entity[i]) : (entity[i] >= '0' && entity[i] <= '9' ? entity[i] - '0' : -1);
    if (digit < 0 || static_cast<uint32_t>(digit) >= base) return 0;
    if (value > (0x10ffff - digit) / base) return 0xfffd;
    value = value * base + digit;
  }
  return xmlScalar(value);
}

enum class Encoding { Utf8, Latin1, Windows1252 };
constexpr uint16_t WINDOWS_1252[] = {0x20ac, 0xfffd, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
                                     0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0xfffd, 0x017d, 0xfffd,
                                     0xfffd, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
                                     0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0xfffd, 0x017e, 0x0178};
uint32_t singleByte(uint8_t c, Encoding encoding) {
  return encoding == Encoding::Windows1252 && c >= 0x80 && c < 0xa0 ? WINDOWS_1252[c - 0x80] : c;
}
uint32_t decodeUtf8(std::string_view text, size_t& i, Encoding encoding) {
  uint32_t c = static_cast<uint8_t>(text[i++]);
  if (encoding != Encoding::Utf8) return singleByte(c, encoding);
  if (c < 0x80) return c;
  unsigned count = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
  if (count == 0) return 0xfffd;
  const uint32_t minimum = count == 1 ? 0x80 : count == 2 ? 0x800 : 0x10000;
  c &= (1u << (6 - count)) - 1;
  while (count--) {
    if (i == text.size() || (static_cast<uint8_t>(text[i]) & 0xc0) != 0x80) return 0xfffd;
    c = (c << 6) | (static_cast<uint8_t>(text[i++]) & 63);
  }
  return c < minimum ? 0xfffd : xmlScalar(c);
}

struct Tag {
  const char* source;
  const char* output;
  bool block;
};
constexpr Tag TAGS[] = {{"p", "p", true},           {"div", "div", true},       {"h1", "h1", true},
                        {"h2", "h2", true},         {"h3", "h3", true},         {"h4", "h4", true},
                        {"h5", "h5", true},         {"h6", "h6", true},         {"blockquote", "blockquote", true},
                        {"ul", "ul", true},         {"ol", "ol", true},         {"li", "li", true},
                        {"dl", "div", true},        {"dt", "div", true},        {"dd", "div", true},
                        {"table", "table", true},   {"thead", "thead", true},   {"tbody", "tbody", true},
                        {"tfoot", "tfoot", true},   {"tr", "tr", true},         {"td", "td", true},
                        {"th", "th", true},         {"caption", "caption", true}, {"pre", "div", true},
                        {"section", "div", true},   {"article", "div", true},   {"main", "div", true},
                        {"header", "div", true},    {"footer", "div", true},    {"aside", "div", true},
                        {"nav", "div", true},       {"figure", "div", true},    {"figcaption", "p", true},
                        {"b", "b", false},          {"strong", "strong", false}, {"i", "i", false},
                        {"em", "em", false},        {"u", "u", false},          {"ins", "ins", false},
                        {"s", "s", false},          {"strike", "s", false},     {"del", "del", false},
                        {"sup", "sup", false},      {"sub", "sub", false},      {"a", "a", false},
                        {"span", "span", false},    {"code", "span", false},    {"small", "span", false}};
int tagIndex(std::string_view name) {
  for (size_t i = 0; i < sizeof(TAGS) / sizeof(TAGS[0]); ++i) {
    if (name == TAGS[i].source) return static_cast<int>(i);
  }
  return -1;
}
bool heading(std::string_view name) { return name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6'; }

class Converter {
  ConvertedEpub& book;
  HalFile& input;
  const std::string& sourcePath;
  uint8_t readBuffer[1024];
  size_t readPosition = 0;
  size_t readSize = 0;
  uint64_t remaining;
  char tag[MAX_TAG];
  size_t tagLength = 0;
  bool tagTruncated = false;
  char attribute[MAX_PATH];
  char secondary[MAX_PATH];
  char title[256] = {};
  char author[256] = {};
  char language[40] = {};
  size_t titleLength = 0;
  char headingTitle[256] = {};
  size_t headingLength = 0;
  char headingAnchor[192] = {};
  int headingDepth = -1;
  uint8_t stack[MAX_DEPTH];
  size_t depth = 0;
  unsigned images = 0;
  unsigned headings = 0;
  bool inHead = false;
  bool inTitle = false;
  bool ok = true;
  Encoding encoding = Encoding::Utf8;
  std::string imagePath;

  int peek() {
    if (readPosition == readSize) {
      if (!remaining || !ok) return -1;
      const int count = input.read(readBuffer, std::min<uint64_t>(sizeof(readBuffer), remaining));
      if (count <= 0) {
        ok = false;
        return -1;
      }
      readSize = count;
      readPosition = 0;
      remaining -= count;
      vTaskDelay(1);
    }
    return readBuffer[readPosition];
  }
  int next() {
    const int c = peek();
    if (c >= 0) ++readPosition;
    return c;
  }
  void raw(std::string_view text) {
    if (ok && book.content().write(reinterpret_cast<const uint8_t*>(text.data()), text.size()) != text.size()) ok = false;
  }
  void escaped(std::string_view text) {
    if (ok && !ConvertedEpub::writeXml(book.content(), text)) ok = false;
  }
  void scalar(uint32_t c) {
    c = xmlScalar(c);
    if (c == 0xfeff) return;
    if (inTitle) {
      appendScalar(title, sizeof(title), titleLength, c, true);
      return;
    }
    if (inHead) return;
    if (headingDepth >= 0) appendScalar(headingTitle, sizeof(headingTitle), headingLength, c, true);
    if (c == '\n' && findOpen("pre") >= 0) {
      raw("<br/>");
      return;
    }
    char text[4];
    const size_t count = encode(c, text);
    escaped(std::string_view(text, count));
  }
  void textByte(int c) {
    if (c == '&') {
      char entity[34] = {'&'};
      size_t length = 1;
      while (length + 1 < sizeof(entity)) {
        const int v = peek();
        if (v < 0 || !(alpha(v) || (v >= '0' && v <= '9') || v == '#' || v == ';')) break;
        entity[length++] = next();
        if (v == ';') break;
      }
      entity[length] = '\0';
      if (const uint32_t numeric = numericEntity(std::string_view(entity, length))) {
        scalar(numeric);
      } else if (const char* decoded = lookupHtmlEntity(entity, length)) {
        const std::string_view text(decoded);
        for (size_t i = 0; i < text.size();) scalar(decodeUtf8(text, i, Encoding::Utf8));
      } else {
        for (size_t i = 0; i < length; ++i) scalar(entity[i]);
      }
      return;
    }
    if (c < 0x80 || encoding != Encoding::Utf8) {
      scalar(singleByte(c, encoding));
      return;
    }
    char text[4] = {static_cast<char>(c)};
    size_t count = 1;
    const size_t wanted = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 1;
    while (count < wanted && peek() >= 0 && (peek() & 0xc0) == 0x80) text[count++] = next();
    size_t i = 0;
    scalar(decodeUtf8(std::string_view(text, count), i, Encoding::Utf8));
  }
  bool getAttribute(const char* wanted, char* out, size_t capacity) {
    out[0] = '\0';
    if (tagTruncated) return false;
    size_t pos = 0;
    while (pos < tagLength && !space(tag[pos]) && tag[pos] != '/') ++pos;
    while (pos < tagLength) {
      while (pos < tagLength && (space(tag[pos]) || tag[pos] == '/')) ++pos;
      const size_t begin = pos;
      while (pos < tagLength && !space(tag[pos]) && tag[pos] != '=' && tag[pos] != '/') ++pos;
      const std::string_view name(tag + begin, pos - begin);
      while (pos < tagLength && space(tag[pos])) ++pos;
      size_t valueBegin = pos;
      size_t valueEnd = pos;
      if (pos < tagLength && tag[pos] == '=') {
        ++pos;
        while (pos < tagLength && space(tag[pos])) ++pos;
        const char quote = pos < tagLength && (tag[pos] == '\'' || tag[pos] == '"') ? tag[pos++] : 0;
        valueBegin = pos;
        while (pos < tagLength && (quote ? tag[pos] != quote : !space(tag[pos]))) ++pos;
        valueEnd = pos;
        if (quote && pos < tagLength) ++pos;
      }
      if (equal(name, wanted)) return decodeAttribute(std::string_view(tag + valueBegin, valueEnd - valueBegin), out, capacity);
      if (pos == begin) ++pos;
    }
    return false;
  }
  bool decodeAttribute(std::string_view text, char* out, size_t capacity) {
    size_t length = 0;
    out[0] = '\0';
    for (size_t i = 0; i < text.size();) {
      if (text[i] == '&') {
        const size_t end = text.find(';', i + 1);
        if (end != std::string_view::npos && end - i < 33) {
          const auto entity = text.substr(i, end - i + 1);
          if (const uint32_t c = numericEntity(entity)) {
            if (length + 4 >= capacity) return false;
            appendScalar(out, capacity, length, c, false);
            i = end + 1;
            continue;
          }
          if (const char* decoded = lookupHtmlEntity(entity.data(), entity.size())) {
            const size_t count = strlen(decoded);
            if (length + count >= capacity) return false;
            memcpy(out + length, decoded, count);
            length += count;
            out[length] = '\0';
            i = end + 1;
            continue;
          }
        }
      }
      const uint32_t c = decodeUtf8(text, i, encoding);
      char encoded[4];
      if (length + encode(c, encoded) >= capacity) return false;
      appendScalar(out, capacity, length, c, false);
    }
    return true;
  }
  void outputAttribute(const char* name, const char* value) {
    raw(" ");
    raw(name);
    raw("=\"");
    escaped(value);
    raw("\"");
  }
  int findOpen(std::string_view name) const {
    for (size_t i = depth; i > 0; --i) {
      if (name == TAGS[stack[i - 1]].source) return static_cast<int>(i - 1);
    }
    return -1;
  }
  void closeTo(size_t target) {
    while (depth > target) {
      --depth;
      raw("</");
      raw(TAGS[stack[depth]].output);
      raw(">");
      if (static_cast<int>(depth) == headingDepth) {
        if (headingLength && headingTitle[headingLength - 1] == ' ') headingTitle[--headingLength] = '\0';
        if (headingLength && !book.addTocEntry(headingTitle, headingAnchor)) ok = false;
        headingDepth = -1;
      }
    }
  }
  void closeOpen(std::string_view name) {
    const int index = findOpen(name);
    if (index >= 0) closeTo(index);
  }
  void skipDeclaration() {
    if (peek() == '-') {
      next();
      if (peek() == '-') {
        next();
        unsigned dashes = 0;
        for (int c; (c = next()) >= 0;) {
          if (c == '>' && dashes >= 2) return;
          dashes = c == '-' ? std::min(dashes + 1, 2u) : 0;
        }
        return;
      }
    }
    char quote = 0;
    unsigned brackets = 0;
    for (int c; (c = next()) >= 0;) {
      if (quote) {
        if (c == quote) quote = 0;
      } else if (c == '\'' || c == '"') {
        quote = c;
      } else if (c == '[') {
        ++brackets;
      } else if (c == ']' && brackets) {
        --brackets;
      } else if (c == '>' && !brackets) {
        return;
      }
    }
  }
  void skipRaw(std::string_view name) {
    // HTML raw-text elements close at </name>, regardless of JavaScript/CSS quoting.
    for (int c; (c = next()) >= 0;) {
      if (c != '<' || peek() != '/') continue;
      next();
      size_t matched = 0;
      while (matched < name.size() && peek() >= 0 && lower(peek()) == name[matched]) {
        next();
        ++matched;
      }
      if (matched != name.size() || !(peek() == '>' || space(peek()) || peek() == '/')) continue;
      while ((c = next()) >= 0 && c != '>') {}
      return;
    }
  }
  void readTag() {
    tagLength = 0;
    tagTruncated = false;
    char quote = 0;
    for (int c; (c = next()) >= 0;) {
      if (!quote && c == '>') break;
      if (!quote && (c == '\'' || c == '"')) quote = c;
      else if (quote && c == quote) quote = 0;
      if (tagLength + 1 < sizeof(tag)) tag[tagLength++] = c;
      else tagTruncated = true;
    }
    tag[tagLength] = '\0';
  }
  void metadata() {
    if (getAttribute("charset", attribute, sizeof(attribute))) setEncoding(attribute);
    if (!getAttribute("name", attribute, sizeof(attribute))) {
      if (!getAttribute("http-equiv", attribute, sizeof(attribute)) || !equal(attribute, "content-type")) return;
      if (!getAttribute("content", secondary, sizeof(secondary))) return;
      const size_t length = strlen(secondary);
      for (size_t i = 0; i + 8 <= length; ++i) {
        if (equal(std::string_view(secondary + i, 8), "charset=")) {
          char* p = secondary + i + 8;
          while (space(*p) || *p == '\'' || *p == '"') ++p;
          char* end = p;
          while (*end && !space(*end) && *end != ';' && *end != '\'' && *end != '"') ++end;
          *end = '\0';
          setEncoding(p);
          break;
        }
      }
      return;
    }
    if (equal(attribute, "author") && !author[0]) getAttribute("content", author, sizeof(author));
    if ((equal(attribute, "language") || equal(attribute, "dc.language")) && !language[0]) {
      getAttribute("content", language, sizeof(language));
    }
  }
  void setEncoding(std::string_view name) {
    while (!name.empty() && space(name.front())) name.remove_prefix(1);
    while (!name.empty() && space(name.back())) name.remove_suffix(1);
    if (equal(name, "utf-8") || equal(name, "utf8") || equal(name, "us-ascii")) encoding = Encoding::Utf8;
    else if (equal(name, "windows-1252") || equal(name, "cp1252")) encoding = Encoding::Windows1252;
    else if (equal(name, "iso-8859-1") || equal(name, "latin1")) encoding = Encoding::Latin1;
    else {
      LOG_ERR("HTML", "Unsupported document encoding: %.*s", static_cast<int>(name.size()), name.data());
      ok = false;
    }
  }
  bool resolveImage(const char* value) {
    // Strip URL query/fragment before decoding; never turn a URL into an SD path.
    size_t length = 0;
    for (size_t i = 0; value[i] && value[i] != '?' && value[i] != '#'; ++i) {
      unsigned char c = value[i];
      if (c == '%') {
        if (!value[i + 1] || !value[i + 2] || hex(value[i + 1]) < 0 || hex(value[i + 2]) < 0) return false;
        c = (hex(value[i + 1]) << 4) | hex(value[i + 2]);
        i += 2;
      }
      if (c < 0x20 || c == 0x7f || c == ':' || c == '\\' || length + 1 == sizeof(secondary)) return false;
      secondary[length++] = c;
    }
    secondary[length] = '\0';
    if (!length || (secondary[0] == '/' && secondary[1] == '/')) return false;
    imagePath.clear();
    if (secondary[0] != '/') {
      const size_t slash = sourcePath.find_last_of('/');
      if (slash != std::string::npos) {
        if (slash >= MAX_PATH) return false;
        imagePath.assign(sourcePath, 0, slash);
      }
    }
    if (imagePath.size() >= MAX_PATH) return false;
    size_t pos = 0;
    while (pos < length) {
      while (pos < length && secondary[pos] == '/') ++pos;
      const size_t begin = pos;
      while (pos < length && secondary[pos] != '/') ++pos;
      const std::string_view part(secondary + begin, pos - begin);
      if (part.empty() || part == ".") continue;
      if (part == "..") {
        const size_t slash = imagePath.find_last_of('/');
        if (slash == std::string::npos) return false;
        imagePath.resize(slash);
        continue;
      }
      if (imagePath.size() + 1 + part.size() >= MAX_PATH) return false;
      imagePath.push_back('/');
      imagePath.append(part.data(), part.size());
    }
    return !imagePath.empty();
  }
  void image() {
    bool included = false;
    if (images < MAX_IMAGES && getAttribute("src", attribute, sizeof(attribute)) && resolveImage(attribute)) {
      const char* extension = nullptr;
      const char* mediaType = nullptr;
      {
        HalFile file;
        if (Storage.openFileForRead("HTML", imagePath, file) && !file.isDirectory()) {
          uint8_t signature[8] = {};
          const int count = file.read(signature, sizeof(signature));
          static constexpr uint8_t PNG_SIGNATURE[] = {137, 80, 78, 71, 13, 10, 26, 10};
          if (count == 8 && memcmp(signature, PNG_SIGNATURE, sizeof(signature)) == 0) {
            extension = "png";
            mediaType = "image/png";
          } else if (count >= 3 && signature[0] == 0xff && signature[1] == 0xd8 && signature[2] == 0xff) {
            extension = "jpg";
            mediaType = "image/jpeg";
          }
        }
      }
      if (extension) {
        char itemName[48];
        snprintf(itemName, sizeof(itemName), "images/html-%u.%s", ++images, extension);
        if (!book.addImage(imagePath, itemName, mediaType)) {
          ok = false;
          return;
        }
        raw("<img");
        outputAttribute("src", itemName);
        raw(" alt=\"");
        if (getAttribute("alt", attribute, sizeof(attribute))) escaped(attribute);
        raw("\"/>");
        included = true;
      }
    }
    if (!included && getAttribute("alt", attribute, sizeof(attribute)) && attribute[0]) {
      raw(" ");
      escaped(attribute);
      raw(" ");
    }
  }
  void processTag() {
    size_t pos = tag[0] == '/' ? 1 : 0;
    const bool closing = pos != 0;
    const size_t begin = pos;
    while (pos < tagLength && (alpha(tag[pos]) || (tag[pos] >= '0' && tag[pos] <= '9') || tag[pos] == '-')) {
      tag[pos] = lower(tag[pos]);
      ++pos;
    }
    const std::string_view name(tag + begin, pos - begin);
    if (name.empty()) return;
    if (name == "script" || name == "style" || name == "iframe") {
      if (!closing) skipRaw(name);
      return;
    }
    if (name == "head") {
      inHead = !closing;
      if (closing) inTitle = false;
      return;
    }
    if (name == "body") {
      inHead = false;
      inTitle = false;
      return;
    }
    if (name == "title") {
      inTitle = !closing && titleLength == 0;
      return;
    }
    if (name == "meta" && !closing) {
      metadata();
      return;
    }
    if (name == "html") {
      if (!closing && !language[0]) getAttribute("lang", language, sizeof(language));
      return;
    }
    if (inHead || inTitle) return;
    if (name == "br" || name == "hr") {
      if (!closing) {
        raw(name == "br" ? "<br/>" : "<hr/>");
        if (headingDepth >= 0) appendScalar(headingTitle, sizeof(headingTitle), headingLength, ' ', true);
      }
      return;
    }
    if (name == "img") {
      if (!closing) image();
      return;
    }
    const int index = tagIndex(name);
    if (index < 0) return;
    if (closing) {
      closeOpen(name);
      return;
    }
    if (TAGS[index].block) closeOpen("p");
    if (heading(name) && headingDepth >= 0) closeTo(headingDepth);
    if (name == "li") {
      const int item = findOpen("li");
      const int list = std::max(findOpen("ul"), findOpen("ol"));
      if (item > list) closeTo(item);
    }
    if (name == "dt" || name == "dd") {
      const int item = std::max(findOpen("dt"), findOpen("dd"));
      if (item > findOpen("dl")) closeTo(item);
    }
    if (name == "tr" || name == "td" || name == "th") {
      const int element = name == "tr" ? findOpen("tr") : std::max(findOpen("td"), findOpen("th"));
      if (element > findOpen("table")) closeTo(element);
    }
    if (depth == MAX_DEPTH) return;
    raw("<");
    raw(TAGS[index].output);
    const bool hasId = getAttribute("id", attribute, sizeof(headingAnchor)) && attribute[0];
    if (hasId) outputAttribute("id", attribute);
    if (heading(name) && headings < MAX_HEADINGS) {
      headingDepth = static_cast<int>(depth);
      headingLength = 0;
      headingTitle[0] = '\0';
      if (hasId) memcpy(headingAnchor, attribute, strlen(attribute) + 1);
      else {
        snprintf(headingAnchor, sizeof(headingAnchor), "crosspoint-html-heading-%u", headings + 1);
        outputAttribute("id", headingAnchor);
      }
      ++headings;
    }
    if (name == "a" && getAttribute("href", attribute, sizeof(attribute)) && attribute[0] == '#') {
      outputAttribute("href", attribute);
    }
    if (getAttribute("dir", attribute, sizeof(attribute)) && (equal(attribute, "rtl") || equal(attribute, "ltr"))) {
      outputAttribute("dir", attribute);
    }
    if (name == "td" || name == "th") {
      for (const char* attr : {"colspan", "rowspan"}) {
        if (getAttribute(attr, attribute, sizeof(attribute))) {
          const size_t length = strlen(attribute);
          bool numeric = length > 0 && length <= 3;
          for (size_t i = 0; numeric && i < length; ++i) numeric = attribute[i] >= '0' && attribute[i] <= '9';
          if (numeric) outputAttribute(attr, attribute);
        }
      }
    }
    raw(">");
    stack[depth++] = index;
    if (tagLength && tag[tagLength - 1] == '/') closeTo(depth - 1);
  }

 public:
  Converter(ConvertedEpub& book, HalFile& input, const std::string& sourcePath)
      : book(book), input(input), sourcePath(sourcePath), remaining(input.fileSize64()) {
    // One bounded cold-path string is reused for every local image.
    imagePath.reserve(MAX_PATH);
  }
  bool run() {
    peek();
    if (readSize >= 2 && ((readBuffer[0] == 0xff && readBuffer[1] == 0xfe) ||
                          (readBuffer[0] == 0xfe && readBuffer[1] == 0xff) ||
                          (readBuffer[0] == 0 && readBuffer[1] == 0))) {
      LOG_ERR("HTML", "UTF-16/32 HTML must be saved as UTF-8");
      return false;
    }
    for (int c; ok && (c = next()) >= 0;) {
      if (c != '<') {
        textByte(c);
        continue;
      }
      if (peek() == '!' || peek() == '?') {
        next();
        skipDeclaration();
      } else if (alpha(peek()) || peek() == '/') {
        readTag();
        processTag();
      } else {
        scalar('<');
      }
    }
    closeTo(0);
    if (titleLength && title[titleLength - 1] == ' ') title[--titleLength] = '\0';
    if (titleLength) book.setTitle(title);
    if (author[0]) book.setAuthor(author);
    size_t subtagLength = 0;
    bool validLanguage = alpha(language[0]);
    for (size_t i = 0; language[i] && validLanguage; ++i) {
      if (language[i] == '-') {
        validLanguage = subtagLength != 0;
        subtagLength = 0;
      } else {
        validLanguage = (alpha(language[i]) || (language[i] >= '0' && language[i] <= '9')) && ++subtagLength <= 8;
      }
    }
    if (validLanguage && subtagLength) book.setLanguage(language);
    return ok && book.good();
  }
};
}  // namespace

std::string Html::prepareEpub(const std::string& sourcePath, const std::string& cacheRoot) {
  // The package writer and ~6 KB tokenizer are temporary heap allocations, not task-stack buffers.
  auto book = makeUniqueNoThrow<ConvertedEpub>(sourcePath, cacheRoot, "html", FORMAT_VERSION);
  if (!book) {
    LOG_ERR("HTML", "Unable to allocate EPUB converter");
    return {};
  }
  const auto preparation = book->prepare();
  if (preparation == ConvertedEpub::Preparation::Cached) return book->path();
  if (preparation == ConvertedEpub::Preparation::Failed) return {};
  HalFile source;
  if (!Storage.openFileForRead("HTML", sourcePath, source) || source.isDirectory() || !book->begin()) return {};
  auto converter = makeUniqueNoThrow<Converter>(*book, source, sourcePath);
  if (!converter) {
    LOG_ERR("HTML", "Unable to allocate %u-byte HTML tokenizer", static_cast<unsigned>(sizeof(Converter)));
    return {};
  }
  if (!converter->run() || !book->finish()) {
    LOG_ERR("HTML", "Conversion failed: %s", sourcePath.c_str());
    return {};
  }
  return book->path();
}
