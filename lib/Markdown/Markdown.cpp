#include "Markdown.h"

#include <Epub/htmlEntities.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "ConvertedEpub.h"

namespace {
constexpr uint32_t FORMAT_VERSION = 1;
constexpr size_t LINE_SIZE = 2048;

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
  return text;
}
size_t runOf(std::string_view text, char c) {
  size_t count = 0;
  while (count < text.size() && text[count] == c) ++count;
  return count;
}
bool rule(std::string_view text, char marker, size_t minimum = 3) {
  size_t count = 0;
  for (const char c : text) {
    if (c == marker) ++count;
    else if (c != ' ' && c != '\t') return false;
  }
  return count >= minimum;
}
size_t findClosing(std::string_view text, std::string_view delimiter, size_t start) {
  while (start < text.size()) {
    const size_t next = text.find(delimiter, start);
    if (next == std::string_view::npos) return next;
    size_t escapes = 0;
    for (size_t i = next; i && text[i - 1] == '\\'; --i) ++escapes;
    if (!(escapes & 1)) return next;
    start = next + delimiter.size();
  }
  return std::string_view::npos;
}
bool tableDivider(std::string_view line) {
  if (line.find('|') == std::string_view::npos) return false;
  line = trim(line);
  if (!line.empty() && line.front() == '|') line.remove_prefix(1);
  if (!line.empty() && line.back() == '|') line.remove_suffix(1);
  size_t cells = 0;
  while (!line.empty()) {
    const size_t end = line.find('|');
    auto cell = trim(line.substr(0, end));
    if (!cell.empty() && cell.front() == ':') cell.remove_prefix(1);
    if (!cell.empty() && cell.back() == ':') cell.remove_suffix(1);
    if (!rule(cell, '-')) return false;
    ++cells;
    if (end == std::string_view::npos) break;
    line.remove_prefix(end + 1);
  }
  return cells != 0;
}

class MarkdownConversion {
 public:
  MarkdownConversion(ConvertedEpub& book, const std::string& path) : book(book), source(path) {}
  bool run() {
    if (!Storage.openFileForRead("MD", source, input)) return false;
    if (!frontmatter()) return false;
    bool have = readLine(current, currentLength, currentMore);
    bool continuation = false;
    while (have && ok) {
      size_t nextLength = 0;
      bool nextMore = false;
      bool nextHave = readLine(next, nextLength, nextMore);
      bool consumeNext = false;
      const auto line = std::string_view(current, currentLength);
      process(line, std::string_view(next, nextLength), continuation, currentMore, nextHave && !nextMore, consumeNext);
      continuation = currentMore;
      if (consumeNext) {
        have = readLine(current, currentLength, currentMore);
        continuation = false;
      } else {
        memcpy(current, next, nextLength + 1);
        currentLength = nextLength;
        currentMore = nextMore;
        have = nextHave;
      }
      if (++lineCount % 32 == 0) vTaskDelay(1);
    }
    closeParagraph();
    closeList();
    if (table) raw("</table>");
    if (fence) raw("</div>");
    return ok && book.good() && !readFailed;
  }

 private:
  ConvertedEpub& book;
  const std::string& source;
  HalFile input;
  char current[LINE_SIZE + 4];
  char next[LINE_SIZE + 4];
  uint8_t io[1024];
  size_t ioPosition = 0;
  size_t ioSize = 0;
  size_t currentLength = 0;
  bool currentMore = false;
  bool readFailed = false;
  bool ok = true;
  bool paragraph = false;
  bool table = false;
  char list = 0;
  char fence = 0;
  size_t fenceLength = 0;
  bool explicitTitle = false;
  bool firstHeading = true;
  uint32_t headingCount = 0;
  uint32_t imageCount = 0;
  uint32_t lineCount = 0;

  bool raw(const char* text) {
    const size_t size = strlen(text);
    ok = ok && book.content().write(reinterpret_cast<const uint8_t*>(text), size) == size;
    return ok;
  }
  bool escaped(std::string_view text) { ok = ok && ConvertedEpub::writeXml(book.content(), text); return ok; }
  int byte() {
    if (ioPosition == ioSize) {
      if (input.position() >= input.size()) return -1;
      const int count = input.read(io, sizeof(io));
      if (count <= 0) { readFailed = true; return -1; }
      ioSize = count;
      ioPosition = 0;
    }
    return io[ioPosition++];
  }
  bool rewind() {
    ioSize = ioPosition = 0;
    return input.seek(0);
  }
  bool readLine(char* output, size_t& length, bool& more) {
    length = 0;
    more = false;
    bool any = false;
    while (length < LINE_SIZE) {
      const int c = byte();
      if (c < 0) break;
      any = true;
      if (c == '\r') {
        const int following = byte();
        if (following >= 0 && following != '\n') --ioPosition;
        output[length] = 0;
        return true;
      }
      if (c == '\n') { output[length] = 0; return true; }
      output[length++] = c;
    }
    // Keep UTF-8 sequences intact at a segment boundary. Long physical lines
    // continue as text, never as a fresh heading/list/fence and never disappear.
    if (length == LINE_SIZE) {
      size_t lead = length - 1;
      while (lead && (static_cast<uint8_t>(output[lead]) & 0xc0) == 0x80) --lead;
      const uint8_t c = output[lead];
      const size_t width = c >= 0xc2 && c < 0xe0 ? 2 : c < 0xf0 && c >= 0xe0 ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 1;
      while (length < lead + width) {
        const int nextByte = byte();
        if (nextByte < 0) break;
        output[length++] = nextByte;
      }
      const int following = byte();
      if (following == '\r') {
        const int lf = byte();
        if (lf >= 0 && lf != '\n') --ioPosition;
      } else if (following >= 0 && following != '\n') {
        --ioPosition;
        more = true;
      }
    }
    output[length] = 0;
    return any;
  }

  bool frontmatter() {
    size_t size;
    bool more;
    if (!readLine(current, size, more)) return !readFailed;
    std::string_view first(current, size);
    if (first.substr(0, 3) == "\xef\xbb\xbf") first.remove_prefix(3);
    const bool yaml = trim(first) == "---" && !more;
    bool closed = false;
    if (yaml) {
      for (int n = 0; n < 128 && readLine(current, size, more); ++n) {
        if (!more && (trim(std::string_view(current, size)) == "---" || trim(std::string_view(current, size)) == "...")) {
          closed = true;
          break;
        }
      }
    }
    if (!rewind()) return false;
    if (!closed) {
      // A BOM is transport, not document text. Consume it without affecting the
      // line reader's buffered file cursor.
      uint8_t bom[3];
      if (input.read(bom, 3) == 3 && bom[0] == 0xef && bom[1] == 0xbb && bom[2] == 0xbf) return true;
      return rewind();
    }
    readLine(current, size, more);
    while (readLine(current, size, more)) {
      const auto line = trim(std::string_view(current, size));
      if (line == "---" || line == "...") break;
      const size_t colon = line.find(':');
      if (colon == std::string_view::npos || more) continue;
      auto value = trim(line.substr(colon + 1));
      if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                                (value.front() == '\'' && value.back() == '\''))) {
        value.remove_prefix(1); value.remove_suffix(1);
      }
      const auto key = trim(line.substr(0, colon));
      if (key == "title" && !value.empty()) { book.setTitle(value); explicitTitle = true; }
      else if (key == "author") book.setAuthor(value);
      else if (key == "lang" || key == "language") book.setLanguage(value);
    }
    return !readFailed;
  }

  void closeParagraph() { if (paragraph) { raw("</p>"); paragraph = false; } }
  void closeList() { if (list) { raw(list == 'o' ? "</ol>" : "</ul>"); list = 0; } }
  void blockBoundary() { closeParagraph(); closeList(); if (table) { raw("</table>"); table = false; } }

  void heading(std::string_view label, unsigned level) {
    blockBoundary();
    char tag[8];
    char anchor[40];
    char slug[128];
    snprintf(anchor, sizeof(anchor), "md-heading-%lu", static_cast<unsigned long>(++headingCount));
    size_t used = 0;
    for (unsigned char c : label) {
      if (used == sizeof(slug) - 1) break;
      if (c == ' ' || c == '\t') { if (used && slug[used - 1] != '-') slug[used++] = '-'; }
      else if (c >= 128 || std::isalnum(c) || c == '-' || c == '_') slug[used++] = std::tolower(c);
    }
    if (used) {
      size_t lead = used - 1;
      while (lead && (static_cast<uint8_t>(slug[lead]) & 0xc0) == 0x80) --lead;
      const uint8_t c = slug[lead];
      const size_t width = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 ? 2 : 1;
      if (used - lead < width) used = lead;
    }
    slug[used] = 0;
    raw("<div id=\""); escaped(anchor); raw("\">");
    snprintf(tag, sizeof(tag), "<h%u", level);
    raw(tag);
    if (used) { raw(" id=\""); escaped(slug); raw("\""); }
    raw(">"); inlineText(label); snprintf(tag, sizeof(tag), "</h%u>", level); raw(tag); raw("</div>");
    if (!book.addTocEntry(label, anchor)) ok = false;
    if (firstHeading && !explicitTitle) book.setTitle(label);
    firstHeading = false;
  }

  void codeLine(std::string_view line, bool more, bool continuation) {
    if (!continuation) raw("<p style=\"text-align:left\"><em><code>");
    for (size_t i = 0; i < line.size();) {
      if (line[i] == ' ' || line[i] == '\t') {
        raw(line[i] == '\t' ? "&#160;&#160;&#160;&#160;" : "&#160;"); ++i;
      } else {
        size_t end = line.find_first_of(" \t", i);
        if (end == std::string_view::npos) end = line.size();
        escaped(line.substr(i, end - i)); i = end;
      }
    }
    if (!more) raw("</code></em></p>");
  }

  void tableRow(std::string_view line, bool header) {
    line = trim(line);
    if (!line.empty() && line.front() == '|') line.remove_prefix(1);
    if (!line.empty() && line.back() == '|') line.remove_suffix(1);
    raw("<tr>");
    while (true) {
      const size_t end = findClosing(line, "|", 0);
      raw(header ? "<th>" : "<td>"); inlineText(trim(line.substr(0, end))); raw(header ? "</th>" : "</td>");
      if (end == std::string_view::npos) break;
      line.remove_prefix(end + 1);
    }
    raw("</tr>");
  }

  void process(std::string_view line, std::string_view lookahead, bool continuation, bool more, bool nextComplete,
               bool& consumeNext) {
    auto stripped = trim(line);
    if (fence) {
      if (!continuation && !more && !stripped.empty() && stripped.front() == fence &&
          runOf(stripped, fence) >= fenceLength && trim(stripped.substr(runOf(stripped, fence))).empty()) {
        raw("</div>"); fence = 0;
      } else codeLine(line, more, continuation);
      return;
    }
    if (continuation) {
      inlineText(line);
      if (!more) raw(" ");
      return;
    }
    if (stripped.empty()) { blockBoundary(); return; }
    if (!more && (stripped.front() == '`' || stripped.front() == '~') && runOf(stripped, stripped.front()) >= 3) {
      blockBoundary(); fence = stripped.front(); fenceLength = runOf(stripped, fence); raw("<div>"); return;
    }
    if (!more && nextComplete && (rule(trim(lookahead), '=', 1) || rule(trim(lookahead), '-', 1))) {
      heading(stripped, trim(lookahead).front() == '=' ? 1 : 2); consumeNext = true; return;
    }
    const size_t hashes = runOf(stripped, '#');
    if (!more && hashes && hashes <= 6 && (hashes == stripped.size() || stripped[hashes] == ' ' || stripped[hashes] == '\t')) {
      auto label = trim(stripped.substr(hashes));
      size_t end = label.size();
      while (end && label[end - 1] == '#') --end;
      if (end < label.size() && end && label[end - 1] == ' ') label = trim(label.substr(0, end));
      heading(label, hashes); return;
    }
    if (!more && (rule(stripped, '-') || rule(stripped, '*') || rule(stripped, '_'))) {
      blockBoundary(); raw("<hr/>"); return;
    }
    if (!more && nextComplete && stripped.find('|') != std::string_view::npos && tableDivider(lookahead)) {
      blockBoundary(); raw("<table>"); table = true; tableRow(stripped, true); consumeNext = true; return;
    }
    if (table) {
      if (!more && stripped.find('|') != std::string_view::npos) { tableRow(stripped, false); return; }
      raw("</table>"); table = false;
    }
    if (stripped.front() == '>') {
      blockBoundary();
      while (!stripped.empty() && stripped.front() == '>') { stripped.remove_prefix(1); stripped = trim(stripped); }
      raw("<blockquote><p>"); inlineText(stripped); raw("</p></blockquote>"); return;
    }
    size_t marker = 0;
    char kind = 0;
    if (stripped.size() >= 2 && (stripped[0] == '-' || stripped[0] == '*' || stripped[0] == '+') && stripped[1] == ' ') {
      marker = 2; kind = 'u';
    } else {
      while (marker < stripped.size() && std::isdigit(static_cast<unsigned char>(stripped[marker]))) ++marker;
      if (marker && marker <= 9 && marker + 1 < stripped.size() && (stripped[marker] == '.' || stripped[marker] == ')') &&
          stripped[marker + 1] == ' ') { marker += 2; kind = 'o'; }
    }
    if (kind && !more) {
      closeParagraph();
      if (list != kind) { closeList(); raw(kind == 'o' ? "<ol>" : "<ul>"); list = kind; }
      raw("<li>"); inlineText(trim(stripped.substr(marker))); raw("</li>"); return;
    }
    closeList();
    if (line.size() >= 4 && line.substr(0, 4) == "    " && !paragraph && !more) {
      codeLine(line.substr(4), false, false); return;
    }
    if (!paragraph) { raw("<p>"); paragraph = true; }
    bool hardBreak = !more && line.size() >= 2 && line.substr(line.size() - 2) == "  ";
    if (!more && !line.empty() && line.back() == '\\') { line.remove_suffix(1); hardBreak = true; }
    inlineText(line);
    if (!more) raw(hardBreak ? "<br/>" : " ");
  }

  void image(std::string_view label, std::string_view url, unsigned depth) {
    if (url.empty() || url.size() > 240 || url.find(':') != std::string_view::npos ||
        url.find("..") != std::string_view::npos || url.front() == '/' || url.find('\\') != std::string_view::npos) {
      inlineText(label, depth + 1); return;
    }
    const size_t slash = source.find_last_of('/');
    std::string path = slash == std::string::npos ? "" : source.substr(0, slash + 1);
    path.append(url.data(), url.size());
    const size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot);
    for (char& c : ext) c = std::tolower(static_cast<unsigned char>(c));
    const bool png = ext == ".png";
    if ((!png && ext != ".jpg" && ext != ".jpeg") || !Storage.exists(path.c_str())) {
      inlineText(label, depth + 1); return;
    }
    char item[48];
    snprintf(item, sizeof(item), "images/md-%lu.%s", static_cast<unsigned long>(++imageCount), png ? "png" : "jpg");
    if (!book.addImage(path, item, png ? "image/png" : "image/jpeg")) { ok = false; return; }
    raw("<img src=\""); escaped(item); raw("\" alt=\""); escaped(label); raw("\"/>");
  }

  void inlineText(std::string_view text, unsigned depth = 0) {
    if (depth == 8) { escaped(text); return; }
    size_t pos = 0;
    size_t plain = 0;
    while (pos < text.size() && ok) {
      size_t end = std::string_view::npos;
      size_t width = 0;
      const char c = text[pos];
      const char* open = nullptr;
      const char* close = nullptr;
      if (c == '\\' && pos + 1 < text.size() && std::ispunct(static_cast<unsigned char>(text[pos + 1]))) {
        escaped(text.substr(plain, pos - plain)); escaped(text.substr(pos + 1, 1)); pos += 2; plain = pos; continue;
      }
      if (c == '&') {
        const size_t semicolon = text.find(';', pos + 1);
        if (semicolon != std::string_view::npos && semicolon - pos <= 32) {
          const auto entity = text.substr(pos, semicolon - pos + 1);
          const char* named = lookupHtmlEntity(entity.data(), entity.size());
          char utf8[4];
          size_t utf8Size = 0;
          if (!named && entity.size() > 3 && entity[1] == '#') {
            auto digits = entity.substr(2, entity.size() - 3);
            int base = 10;
            if (!digits.empty() && (digits.front() == 'x' || digits.front() == 'X')) {
              digits.remove_prefix(1); base = 16;
            }
            uint32_t cp = 0;
            const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), cp, base);
            if (!digits.empty() && result.ec == std::errc{} && result.ptr == digits.data() + digits.size() &&
                cp && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff)) {
              if (cp < 0x80) { utf8[0] = cp; utf8Size = 1; }
              else if (cp < 0x800) {
                utf8[0] = 0xc0 | (cp >> 6); utf8[1] = 0x80 | (cp & 63); utf8Size = 2;
              } else if (cp < 0x10000) {
                utf8[0] = 0xe0 | (cp >> 12); utf8[1] = 0x80 | ((cp >> 6) & 63);
                utf8[2] = 0x80 | (cp & 63); utf8Size = 3;
              } else {
                utf8[0] = 0xf0 | (cp >> 18); utf8[1] = 0x80 | ((cp >> 12) & 63);
                utf8[2] = 0x80 | ((cp >> 6) & 63); utf8[3] = 0x80 | (cp & 63); utf8Size = 4;
              }
            }
          }
          if (named || utf8Size) {
            escaped(text.substr(plain, pos - plain));
            escaped(named ? std::string_view(named) : std::string_view(utf8, utf8Size));
            pos = semicolon + 1; plain = pos; continue;
          }
        }
      }
      if (c == '`') {
        width = runOf(text.substr(pos), '`');
        end = findClosing(text, text.substr(pos, width), pos + width);
        if (end != std::string_view::npos) {
          escaped(text.substr(plain, pos - plain)); raw("<code><em>"); escaped(text.substr(pos + width, end - pos - width));
          raw("</em></code>"); pos = end + width; plain = pos; continue;
        }
      }
      const bool isImage = c == '!' && pos + 1 < text.size() && text[pos + 1] == '[';
      if (c == '[' || isImage) {
        const size_t begin = pos + (isImage ? 2 : 1);
        const size_t labelEnd = findClosing(text, "](", begin);
        if (labelEnd != std::string_view::npos) {
          size_t urlEnd = labelEnd + 2;
          unsigned nesting = 1;
          for (; urlEnd < text.size(); ++urlEnd) {
            if (text[urlEnd] == '\\') { ++urlEnd; continue; }
            if (text[urlEnd] == '(') ++nesting;
            else if (text[urlEnd] == ')' && --nesting == 0) break;
          }
          if (urlEnd < text.size()) {
            auto url = trim(text.substr(labelEnd + 2, urlEnd - labelEnd - 2));
            if (!url.empty() && url.front() == '<') {
              const size_t angle = url.find('>');
              if (angle != std::string_view::npos) url = url.substr(1, angle - 1);
            } else {
              const size_t space = url.find_first_of(" \t");
              if (space != std::string_view::npos) url = url.substr(0, space);
            }
            escaped(text.substr(plain, pos - plain));
            const auto label = text.substr(begin, labelEnd - begin);
            if (isImage) image(label, url, depth);
            else { raw("<a href=\""); escaped(url); raw("\">"); inlineText(label, depth + 1); raw("</a>"); }
            pos = urlEnd + 1; plain = pos; continue;
          }
        }
      }
      if (c == '*' || c == '_' || c == '~') {
        width = std::min<size_t>(runOf(text.substr(pos), c), c == '~' ? 2 : 3);
        const bool intraword = c == '_' && pos && pos + width < text.size() &&
            std::isalnum(static_cast<unsigned char>(text[pos - 1])) && std::isalnum(static_cast<unsigned char>(text[pos + width]));
        if (!intraword && (c != '~' || width == 2) && pos + width < text.size() && text[pos + width] != ' ') {
          end = findClosing(text, text.substr(pos, width), pos + width);
          if (end != std::string_view::npos && end > pos + width && text[end - 1] != ' ') {
            open = c == '~' ? "<s>" : width == 3 ? "<strong><em>" : width == 2 ? "<strong>" : "<em>";
            close = c == '~' ? "</s>" : width == 3 ? "</em></strong>" : width == 2 ? "</strong>" : "</em>";
          }
        }
      }
      if (open) {
        escaped(text.substr(plain, pos - plain)); raw(open); inlineText(text.substr(pos + width, end - pos - width), depth + 1);
        raw(close); pos = end + width; plain = pos;
      } else ++pos;
    }
    escaped(text.substr(plain));
  }
};
}  // namespace

std::string Markdown::prepare(const std::string& sourcePath, const std::string& cacheRoot) {
  // Fixed 5 KiB line/I/O state plus the shared 6 KiB writer live off the task
  // stack. Long lines stream through these buffers without accumulating a book.
  auto book = makeUniqueNoThrow<ConvertedEpub>(sourcePath, cacheRoot, "markdown", FORMAT_VERSION);
  if (!book) { LOG_ERR("MD", "No memory for EPUB writer"); return {}; }
  const auto state = book->prepare();
  if (state == ConvertedEpub::Preparation::Cached) return book->path();
  if (state == ConvertedEpub::Preparation::Failed || !book->begin()) return {};
  auto parser = makeUniqueNoThrow<MarkdownConversion>(*book, sourcePath);
  if (!parser) { LOG_ERR("MD", "No memory for Markdown parser"); return {}; }
  if (!parser->run()) { LOG_ERR("MD", "Markdown conversion failed"); return {}; }
  parser.reset();
  if (!book->finish()) return {};
  return book->path();
}
