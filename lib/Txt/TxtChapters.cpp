#include "TxtChapters.h"

#include <cstring>
#include <initializer_list>

#include "TxtLineBreak.h"

namespace TxtChapters {

namespace {

using TxtLineBreak::decodeAt;

bool isTrimSpace(const uint32_t cp) {
  return cp == ' ' || cp == '\t' || cp == '\r' || cp == 0x00A0 || cp == 0x3000 || cp == 0xFEFF;
}

bool isDigit(const uint32_t cp) { return (cp >= '0' && cp <= '9') || (cp >= 0xFF10 && cp <= 0xFF19); }

bool isChineseNumeral(const uint32_t cp) {
  switch (cp) {
    case 0x96F6:  // 零
    case 0x4E00:  // 一
    case 0x4E8C:  // 二
    case 0x4E09:  // 三
    case 0x56DB:  // 四
    case 0x4E94:  // 五
    case 0x516D:  // 六
    case 0x4E03:  // 七
    case 0x516B:  // 八
    case 0x4E5D:  // 九
    case 0x5341:  // 十
    case 0x767E:  // 百
    case 0x5343:  // 千
    case 0x4E07:  // 万
    case 0x842C:  // 萬
    case 0x4E24:  // 两
    case 0x5169:  // 兩
    case 0x3007:  // 〇
    case 0x58F9:  // 壹
    case 0x8D30:  // 贰
    case 0x8CB3:  // 貳
    case 0x53C1:  // 叁
    case 0x53C3:  // 參
    case 0x8086:  // 肆
    case 0x4F0D:  // 伍
    case 0x9646:  // 陆
    case 0x9678:  // 陸
    case 0x67D2:  // 柒
    case 0x634C:  // 捌
    case 0x7396:  // 玖
    case 0x62FE:  // 拾
    case 0x4F70:  // 佰
    case 0x4EDF:  // 仟
      return true;
    default:
      return isDigit(cp);
  }
}

enum class Marker : uint8_t { None, Titled, Separated };

// 章 and 卷 may run straight into the title; the other markers also start
// ordinary words (回合, 部分, 集团, 节日, 篇幅), so a separator or the line end
// has to follow them.
Marker markerKind(const uint32_t cp) {
  switch (cp) {
    case 0x7AE0:  // 章
    case 0x5377:  // 卷
      return Marker::Titled;
    case 0x8282:  // 节
    case 0x7BC0:  // 節
    case 0x56DE:  // 回
    case 0x96C6:  // 集
    case 0x90E8:  // 部
    case 0x7BC7:  // 篇
      return Marker::Separated;
    default:
      return Marker::None;
  }
}

bool isSeparator(const uint32_t cp) {
  switch (cp) {
    case ':':
    case '.':
    case '-':
    case '_':
    case '(':
    case '[':
    case ')':
    case 0x00B7:  // ·
    case 0x2014:  // —
    case 0x201C:  // “
    case 0x3001:  // 、
    case 0x300A:  // 《
    case 0x300C:  // 「
    case 0x3010:  // 【
    case 0x30FB:  // ・
    case 0xFF08:  // （
    case 0xFF0E:  // ．
    case 0xFF1A:  // ：
      return true;
    default:
      return isTrimSpace(cp);
  }
}

// Whether the heading ends at `pos` or continues after a separator.
bool endsOrSeparates(const char* text, const size_t pos, const size_t end) {
  if (pos >= end) return true;
  uint32_t cp = 0;
  decodeAt(text, pos, end, cp);
  return isSeparator(cp);
}

bool matchUtf8(const char* text, const size_t pos, const size_t end, const char* word, size_t& after) {
  const size_t n = std::strlen(word);
  if (end - pos < n || std::memcmp(text + pos, word, n) != 0) return false;
  after = pos + n;
  return true;
}

char lowerAscii(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool matchWordNoCase(const char* text, const size_t pos, const size_t end, const char* word, size_t& after) {
  const size_t n = std::strlen(word);
  if (end - pos < n) return false;
  for (size_t i = 0; i < n; i++) {
    if (lowerAscii(text[pos + i]) != word[i]) return false;
  }
  after = pos + n;
  return true;
}

bool isAsciiLetter(const char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// 第 <numerals> <marker>
bool isNumberedChinese(const char* text, const size_t end) {
  size_t pos = 0;
  if (!matchUtf8(text, pos, end, "\xE7\xAC\xAC", pos)) return false;  // 第
  size_t numerals = 0;
  while (pos < end) {
    uint32_t cp = 0;
    const size_t n = decodeAt(text, pos, end, cp);
    if (!isChineseNumeral(cp)) break;
    pos += n;
    numerals++;
  }
  if (numerals == 0 || numerals > 12 || pos >= end) return false;
  uint32_t cp = 0;
  const size_t n = decodeAt(text, pos, end, cp);
  const Marker marker = markerKind(cp);
  if (marker == Marker::None) return false;
  pos += n;
  return marker == Marker::Titled || endsOrSeparates(text, pos, end);
}

constexpr const char* kNamedSections[] = {
    "\xE5\xBA\x8F\xE7\xAB\xA0",              // 序章
    "\xE5\xBA\x8F\xE8\xA8\x80",              // 序言
    "\xE5\xBA\x8F\xE5\xB9\x95",              // 序幕
    "\xE6\xA5\x94\xE5\xAD\x90",              // 楔子
    "\xE5\xBC\x95\xE5\xAD\x90",              // 引子
    "\xE5\xBC\x95\xE8\xA8\x80",              // 引言
    "\xE5\x89\x8D\xE8\xA8\x80",              // 前言
    "\xE5\xB0\xBE\xE5\xA3\xB0",              // 尾声
    "\xE5\xB0\xBE\xE8\x81\xB2",              // 尾聲
    "\xE7\xBB\x88\xE7\xAB\xA0",              // 终章
    "\xE7\xB5\x82\xE7\xAB\xA0",              // 終章
    "\xE5\x90\x8E\xE8\xAE\xB0",              // 后记
    "\xE5\xBE\x8C\xE8\xA8\x98",              // 後記
    "\xE7\x95\xAA\xE5\xA4\x96",              // 番外
    "\xE5\xA4\x96\xE4\xBC\xA0",              // 外传
    "\xE5\xA4\x96\xE5\x82\xB3",              // 外傳
    "\xE5\xAE\x8C\xE6\x9C\xAC\xE6\x84\x9F\xE8\xA8\x80",  // 完本感言
    "\xE5\xBA\x8F",                          // 序 (after 序章/序言/序幕)
};

bool isNamedSection(const char* text, const size_t end) {
  for (const char* word : kNamedSections) {
    size_t pos = 0;
    if (!matchUtf8(text, 0, end, word, pos)) continue;
    if (endsOrSeparates(text, pos, end)) return true;
    uint32_t cp = 0;
    decodeAt(text, pos, end, cp);
    // 番外一, 番外篇, 尾声二: a numeral or 篇 may follow directly.
    return isChineseNumeral(cp) || cp == 0x7BC7;
  }
  return false;
}

constexpr const char* kNumberWords[] = {"one",      "two",       "three",    "four",     "five",    "six",
                                        "seven",    "eight",     "nine",     "ten",      "eleven",  "twelve",
                                        "thirteen", "fourteen",  "fifteen",  "sixteen",  "seventeen",
                                        "eighteen", "nineteen",  "twenty",   "thirty",   "forty",   "fifty",
                                        "sixty",    "seventy",   "eighty",   "ninety",   "hundred"};

bool isNumberWord(const char* text, const size_t start, const size_t end) {
  for (const char* word : kNumberWords) {
    if (std::strlen(word) == end - start) {
      size_t after = 0;
      if (matchWordNoCase(text, start, end, word, after)) return true;
    }
  }
  return false;
}

bool isRomanNumeral(const char* text, const size_t start, const size_t end) {
  if (end - start > 8) return false;
  for (size_t i = start; i < end; i++) {
    switch (lowerAscii(text[i])) {
      case 'i':
      case 'v':
      case 'x':
      case 'l':
      case 'c':
      case 'd':
      case 'm':
        break;
      default:
        return false;
    }
  }
  return true;
}

// Chapter <digits | roman | number word>, Prologue, Epilogue.
bool isEnglishHeading(const char* text, const size_t end) {
  size_t pos = 0;
  for (const char* word : {"prologue", "epilogue"}) {
    if (matchWordNoCase(text, 0, end, word, pos) && (pos >= end || !isAsciiLetter(text[pos]))) {
      return endsOrSeparates(text, pos, end);
    }
  }
  if (!matchWordNoCase(text, 0, end, "chapter", pos)) return false;
  while (pos < end && text[pos] == ' ') pos++;
  const size_t numberStart = pos;
  if (pos < end && text[pos] >= '0' && text[pos] <= '9') {
    while (pos < end && text[pos] >= '0' && text[pos] <= '9') pos++;
    if (pos - numberStart > 4) return false;
  } else {
    while (pos < end && isAsciiLetter(text[pos])) pos++;
    if (pos == numberStart) return false;
    if (!isRomanNumeral(text, numberStart, pos)) {
      if (!isNumberWord(text, numberStart, pos)) return false;
      // twenty-one
      if (pos < end && text[pos] == '-') {
        const size_t second = pos + 1;
        size_t secondEnd = second;
        while (secondEnd < end && isAsciiLetter(text[secondEnd])) secondEnd++;
        if (secondEnd > second && isNumberWord(text, second, secondEnd)) pos = secondEnd;
      }
    }
    if (numberStart == pos) return false;
  }
  if (!endsOrSeparates(text, pos, end)) return false;
  // "Chapter 3 was the dull one." reads as prose: a titled heading does not end with a full stop.
  const bool hasTitle = pos + 1 < end;
  return !(hasTitle && text[end - 1] == '.');
}

}  // namespace

const char* trimHeading(const char* line, size_t length, size_t& trimmedLength) {
  size_t start = 0;
  while (start < length) {
    uint32_t cp = 0;
    const size_t n = decodeAt(line, start, length, cp);
    if (!isTrimSpace(cp)) break;
    start += n;
  }
  size_t end = length;
  while (end > start) {
    // Step back over one code point.
    size_t lead = end - 1;
    while (lead > start && (static_cast<uint8_t>(line[lead]) & 0xC0) == 0x80) lead--;
    uint32_t cp = 0;
    const size_t n = decodeAt(line, lead, end, cp);
    if (lead + n != end || !isTrimSpace(cp)) break;
    end = lead;
  }
  trimmedLength = end - start;
  return line + start;
}

bool isHeading(const char* line, const size_t length) {
  size_t trimmedLength = 0;
  const char* text = trimHeading(line, length, trimmedLength);
  if (trimmedLength == 0 || trimmedLength > MAX_HEADING_BYTES) return false;

  size_t codepoints = 0;
  uint32_t last = 0;
  for (size_t pos = 0; pos < trimmedLength;) {
    uint32_t cp = 0;
    pos += decodeAt(text, pos, trimmedLength, cp);
    if (cp == 0x3002) return false;  // 。 a sentence, not a heading
    last = cp;
    codepoints++;
  }
  if (codepoints > MAX_HEADING_CODEPOINTS) return false;
  // A heading never trails off into the rest of a sentence.
  if (last == 0xFF0C || last == 0xFF1B || last == 0x3001 || last == ',' || last == ';') return false;

  return isNumberedChinese(text, trimmedLength) || isNamedSection(text, trimmedLength) ||
         isEnglishHeading(text, trimmedLength);
}

}  // namespace TxtChapters
