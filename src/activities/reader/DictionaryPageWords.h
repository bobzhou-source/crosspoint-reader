#pragma once

#include <EpdFontFamily.h>

#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "util/CjkDictionary.h"

class GfxRenderer;
class Page;

// The lookup targets on a laid-out reader page, shared by the dictionary word
// picker and the reader's press-and-hold lookup.
namespace DictionaryPageWords {

// Text points into the page. CJK boxes select one UTF-8 character; Latin
// boxes keep the whole NUL-terminated layout token. y is the line top.
struct WordBox {
  int16_t x;
  int16_t y;
  int16_t width;
  uint16_t row;
  const char* text;
  EpdFontFamily::Style style;
  uint8_t cjkBytes = 0;
  uint16_t run = 0;
};

// Picker taps: the highlight box (+2) plus finger error.
constexpr int TAP_SLOP = 4;
// Press and hold: how far outside a word's box a held finger may land and
// still mean that word (about one space at reading sizes).
constexpr int HOLD_REACH = 8;

// A token is selectable when it has an ASCII alphanumeric or a non-ASCII
// codepoint outside U+2000-U+206F (dashes, bullets and other General
// Punctuation that appear as standalone tokens are not words).
inline bool isSelectableToken(const char* text) {
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p != 0; p++) {
    if (*p < 0x80) {
      if (std::isalnum(*p)) return true;
    } else if (*p == 0xE2 && (p[1] == 0x80 || p[1] == 0x81)) {
      if (p[2] == 0) break;  // truncated sequence: skipping would step past the NUL
      p += 2;                // skip the 3-byte General Punctuation codepoint
    } else {
      return true;
    }
  }
  return false;
}

// Appends the boxes of one layout token drawn at (x, y). A token with Han
// characters gives one box per character; punctuation glued to it ends the
// run without changing the displayed text. Any other token is one box when
// it is selectable. Widths are set by measureToken(). Returns true when a box
// was added.
inline bool appendToken(std::vector<WordBox>& words, const char* text, const int x, const int y, const uint16_t row,
                        const EpdFontFamily::Style style, uint16_t& run) {
  bool hasCjk = false;
  for (const char* p = text; *p;) {
    uint32_t cp;
    const size_t n = CjkDictionary::characterBytes(p, cp);
    hasCjk = hasCjk || CjkDictionary::isCjk(cp);
    p += n;
  }
  WordBox box;
  box.x = static_cast<int16_t>(x);
  box.y = static_cast<int16_t>(y);
  box.width = 0;
  box.row = row;
  box.text = text;
  box.style = style;
  bool added = false;
  if (hasCjk) {
    for (const char* p = text; *p;) {
      uint32_t cp;
      const size_t n = CjkDictionary::characterBytes(p, cp);
      if (CjkDictionary::isCjk(cp)) {
        box.text = p;
        box.cjkBytes = static_cast<uint8_t>(n);
        box.run = run;
        words.push_back(box);
        added = true;
      } else {
        ++run;
      }
      p += n;
    }
  } else {
    ++run;
    if (isSelectableToken(text)) {
      words.push_back(box);
      added = true;
    }
  }
  return added;
}

// Sets the widths of the boxes appendToken() made for `text`, starting at
// words[index], and advances index past them. advance(text, style) returns a
// string's advance width; CJK boxes are also shifted to their character.
template <typename Advance>
void measureToken(std::vector<WordBox>& words, size_t& index, const char* text, const EpdFontFamily::Style style,
                  Advance&& advance) {
  if (index >= words.size()) return;
  if (words[index].text == text && !words[index].cjkBytes) {
    words[index++].width = static_cast<int16_t>(advance(text, style));
    return;
  }
  const size_t length = strlen(text);
  if (words[index].text < text || words[index].text >= text + length) return;  // no box in this token
  int offset = 0;
  for (const char* p = text; *p;) {
    uint32_t cp;
    const size_t n = CjkDictionary::characterBytes(p, cp);
    char character[5] = {};
    memcpy(character, p, n);
    const int width = advance(character, style);
    if (index < words.size() && words[index].text == p) {
      auto& word = words[index++];
      word.x = static_cast<int16_t>(word.x + offset);
      word.width = static_cast<int16_t>(width);
    }
    offset += width;
    p += n;
  }
}

// Index of the word whose box, grown by TAP_SLOP, contains the point; -1 when
// none does. First hit wins.
inline int wordAt(const std::vector<WordBox>& words, const int x, const int y, const int lineHeight) {
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    const WordBox& word = words[i];
    if (x >= word.x - TAP_SLOP && x < word.x + word.width + TAP_SLOP && y >= word.y - TAP_SLOP &&
        y < word.y + lineHeight + TAP_SLOP) {
      return i;
    }
  }
  return -1;
}

// Index of the word nearest a held finger: the box containing the point, or
// else the closest box within HOLD_REACH on each axis. -1 over blank space
// (margins, paragraph gaps, the empty end of a short line).
inline int wordNear(const std::vector<WordBox>& words, const int x, const int y, const int lineHeight) {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    const WordBox& word = words[i];
    const int right = word.x + word.width - 1;
    const int bottom = word.y + lineHeight - 1;
    const int dx = x < word.x ? word.x - x : (x > right ? x - right : 0);
    const int dy = y < word.y ? word.y - y : (y > bottom ? y - bottom : 0);
    if (dx > HOLD_REACH || dy > HOLD_REACH) continue;
    const int distance = dx * dx + dy * dy;
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

// The CJK lookup run starting at words[index]: the following Han characters
// of the same run, up to CjkDictionary::MAX_CHARACTERS. Empty for a Latin
// box. Returns the number of characters.
inline size_t cjkRun(const std::vector<WordBox>& words, const size_t index, char (&run)[CjkDictionary::RUN_BYTES]) {
  run[0] = '\0';
  if (index >= words.size() || !words[index].cjkBytes) return 0;
  size_t bytes = 0;
  size_t characters = 0;
  for (size_t i = index; i < words.size() && characters < CjkDictionary::MAX_CHARACTERS; ++i) {
    const auto& word = words[i];
    if (!word.cjkBytes || word.run != words[index].run) break;
    memcpy(run + bytes, word.text, word.cjkBytes);
    bytes += word.cjkBytes;
    ++characters;
  }
  run[bytes] = '\0';
  return characters;
}

// Every lookup target on `page`, drawn at the margins, in reading order.
// rowCount counts the lines that have at least one target.
void extract(GfxRenderer& renderer, const Page& page, int fontId, int marginLeft, int marginTop,
             std::vector<WordBox>& words, uint16_t& rowCount);

// One laid-out TXT display line: its full UTF-8 text and the absolute
// position the reader drew it at. TXT pages keep text as lines rather than
// EPUB word blocks, so their lookup targets come from this description.
struct LineText {
  const char* text;
  int x;
  int y;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
};

// True for a run that separates words on a TXT line: ASCII space/tab, a
// no-break space, or an ideographic space. `bytes` receives its UTF-8 length.
inline bool isLineSeparator(const char* text, size_t& bytes) {
  const auto* p = reinterpret_cast<const uint8_t*>(text);
  if (p[0] == ' ' || p[0] == '\t') {
    bytes = 1;
    return true;
  }
  if (p[0] == 0xC2 && p[1] == 0xA0) {  // U+00A0 NO-BREAK SPACE
    bytes = 2;
    return true;
  }
  if (p[0] == 0xE3 && p[1] == 0x80 && p[2] == 0x80) {  // U+3000 IDEOGRAPHIC SPACE
    bytes = 3;
    return true;
  }
  return false;
}

// Appends every selectable word on `lines`, in reading order. Tokens are split
// at line separators and copied into `storage`, which stays stable after this
// call so WordBox::text can point into it. `advance(text, length)` returns the
// width of a UTF-8 range and is applied to the line prefix before each token,
// matching how the TXT reader draws the full line. rowCount counts the lines
// that have at least one target.
template <typename Advance>
void extractFromLines(const LineText* lines, const size_t lineCount, std::vector<std::string>& storage,
                      std::vector<WordBox>& words, uint16_t& rowCount, Advance&& advance) {
  words.clear();
  storage.clear();
  rowCount = 0;

  struct Span {
    size_t line;
    size_t offset;
    size_t length;
  };
  std::vector<Span> spans;
  spans.reserve(lineCount * 8);

  // Pass 1: find the tokens and give each one stable storage. Boxes are built
  // only after this vector stops growing, since std::string moves invalidate
  // c_str() pointers (including short-string moves).
  for (size_t line = 0; line < lineCount; ++line) {
    const char* p = lines[line].text;
    while (*p) {
      size_t separatorBytes = 0;
      if (isLineSeparator(p, separatorBytes)) {
        p += separatorBytes;
        continue;
      }
      const char* start = p;
      uint32_t first;
      CjkDictionary::characterBytes(p, first);
      const bool firstCjk = CjkDictionary::isCjk(first);
      const bool firstLatin = first < 0x80 && std::isalnum(static_cast<unsigned char>(first));
      while (*p) {
        size_t bytes = 0;
        if (isLineSeparator(p, bytes)) break;
        uint32_t cp;
        const size_t n = CjkDictionary::characterBytes(p, cp);
        // A Latin run inside a CJK sentence is its own selectable token.
        // Punctuation is not a script boundary: it stays with the CJK span so
        // appendToken() skips it and splits the lookup run there.
        const bool latin = cp < 0x80 && std::isalnum(static_cast<unsigned char>(cp));
        if (p != start && CjkDictionary::isCjk(cp) != firstCjk && (firstLatin || latin)) break;
        p += n;
      }
      spans.push_back({line, static_cast<size_t>(start - lines[line].text), static_cast<size_t>(p - start)});
      if (p == start) p += 1;  // malformed/empty codepoint: never spin
    }
  }

  storage.reserve(spans.size());
  for (const auto& span : spans) {
    storage.emplace_back(lines[span.line].text + span.offset, span.length);
  }

  // Pass 2: place the tokens. A fresh run per token keeps CJK characters of
  // one word together and never merges two words across a line separator.
  size_t tokenIndex = 0;
  for (size_t line = 0; line < lineCount; ++line) {
    const auto& text = lines[line];
    const uint16_t row = rowCount;
    uint16_t run = 0;
    bool rowHasWords = false;
    while (tokenIndex < spans.size() && spans[tokenIndex].line == line) {
      const auto& span = spans[tokenIndex];
      const std::string& token = storage[tokenIndex++];
      const int x = text.x + advance(text.text, span.offset);
      uint16_t tokenRun = run++;
      const size_t previous = words.size();
      if (appendToken(words, token.c_str(), x, text.y, row, text.style, tokenRun)) {
        size_t measureIndex = previous;
        measureToken(words, measureIndex, token.c_str(), text.style,
                     [&advance](const char* value, const EpdFontFamily::Style) {
                       return advance(value, strlen(value));
                     });
        rowHasWords = true;
      }
    }
    if (rowHasWords) ++rowCount;
  }
}

}  // namespace DictionaryPageWords
