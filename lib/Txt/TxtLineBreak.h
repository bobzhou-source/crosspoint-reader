#pragma once

#include <Utf8.h>

#include <cstddef>
#include <cstdint>

// Code point decoding and line-break classes for plain-text layout.
namespace TxtLineBreak {

inline constexpr uint32_t REPLACEMENT = 0xFFFD;

// Decodes the code point at text[pos, end). A malformed or truncated sequence
// decodes as one U+FFFD byte, so every byte is consumed exactly once.
inline size_t decodeAt(const char* text, const size_t pos, const size_t end, uint32_t& cp) {
  const auto* p = reinterpret_cast<const uint8_t*>(text) + pos;
  const uint8_t b0 = p[0];
  if (b0 < 0x80) {
    cp = b0;
    return 1;
  }
  size_t n = 0;
  uint32_t value = 0;
  uint8_t lo = 0x80;
  uint8_t hi = 0xBF;
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    n = 2;
    value = b0 & 0x1F;
  } else if (b0 >= 0xE0 && b0 <= 0xEF) {
    n = 3;
    value = b0 & 0x0F;
    if (b0 == 0xE0) lo = 0xA0;  // overlong
    if (b0 == 0xED) hi = 0x9F;  // surrogates
  } else if (b0 >= 0xF0 && b0 <= 0xF4) {
    n = 4;
    value = b0 & 0x07;
    if (b0 == 0xF0) lo = 0x90;  // overlong
    if (b0 == 0xF4) hi = 0x8F;  // above U+10FFFF
  } else {
    cp = REPLACEMENT;
    return 1;
  }
  if (end - pos < n) {
    cp = REPLACEMENT;
    return 1;
  }
  for (size_t i = 1; i < n; i++) {
    const uint8_t b = p[i];
    if (b < (i == 1 ? lo : 0x80) || b > (i == 1 ? hi : 0xBF)) {
      cp = REPLACEMENT;
      return 1;
    }
    value = (value << 6) | (b & 0x3F);
  }
  cp = value;
  return n;
}

// Length of text[0, length) without a trailing UTF-8 sequence that the
// following bytes would complete: a window that ends inside a code point
// stops before it.
inline size_t completePrefix(const char* text, const size_t length) {
  const auto* p = reinterpret_cast<const uint8_t*>(text);
  const size_t lookback = length < 3 ? length : 3;
  for (size_t back = 1; back <= lookback; back++) {
    const size_t i = length - back;
    const uint8_t b = p[i];
    if ((b & 0xC0) == 0x80) continue;  // continuation byte: keep looking for the lead
    size_t need = 0;
    if (b >= 0xC2 && b <= 0xDF) {
      need = 2;
    } else if (b >= 0xE0 && b <= 0xEF) {
      need = 3;
    } else if (b >= 0xF0 && b <= 0xF4) {
      need = 4;
    }
    return need > back ? i : length;
  }
  return length;
}

// Han, kana, bopomofo, CJK punctuation and fullwidth forms: text with an
// implicit break opportunity between characters. Hangul is excluded because
// Korean separates words with spaces.
inline bool isIdeographic(const uint32_t cp) {
  if (cp < 0x2E80) return false;
  if (utf8IsHangul(cp)) return false;
  return utf8IsCjkBreakable(cp) || (cp >= 0x2E80 && cp <= 0x2FDF)  // radicals
         || (cp >= 0x3100 && cp <= 0x312F)                         // bopomofo
         || (cp >= 0x31A0 && cp <= 0x31FF)                         // bopomofo ext, strokes, kana ext
         || (cp >= 0x3200 && cp <= 0x33FF)                         // enclosed CJK, compatibility
         || (cp >= 0x2B740 && cp <= 0x2FA1F)                       // extensions D-F, compat supplement
         || (cp >= 0x30000 && cp <= 0x3134F);                      // extension G
}

// Marks that attach to the preceding code point and never start a line.
inline bool isClusterExtender(const uint32_t cp) {
  return utf8IsCombiningMark(cp) || cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F) ||
         (cp >= 0xE0100 && cp <= 0xE01EF) || (cp >= 0x1F3FB && cp <= 0x1F3FF);
}

// Kinsoku: closing punctuation, small kana and iteration marks never start a line.
inline bool isLineStartProhibited(const uint32_t cp) {
  switch (cp) {
    case ')':
    case ']':
    case '}':
    case ',':
    case '.':
    case ':':
    case ';':
    case '?':
    case '!':
    case '%':
    case 0x00B0:  // °
    case 0x00B7:  // ·
    case 0x2019:  // ’
    case 0x201D:  // ”
    case 0x2025:  // ‥
    case 0x2026:  // …
    case 0x2030:  // ‰
    case 0x2032:  // ′
    case 0x2033:  // ″
    case 0x203C:  // ‼
    case 0x2047:  // ⁇
    case 0x2048:  // ⁈
    case 0x2049:  // ⁉
    case 0x2103:  // ℃
    case 0x22EF:  // ⋯
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0x3005:  // 々
    case 0x3009:  // 〉
    case 0x300B:  // 》
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0x3011:  // 】
    case 0x3015:  // 〕
    case 0x3017:  // 〗
    case 0x3019:  // 〙
    case 0x301B:  // 〛
    case 0x301C:  // 〜
    case 0x301E:  // 〞
    case 0x301F:  // 〟
    case 0x303B:  // 〻
    case 0x309D:  // ゝ
    case 0x309E:  // ゞ
    case 0x30A0:  // ゠
    case 0x30FB:  // ・
    case 0x30FC:  // ー
    case 0x30FD:  // ヽ
    case 0x30FE:  // ヾ
    case 0xFE50:  // ﹐
    case 0xFE51:  // ﹑
    case 0xFE52:  // ﹒
    case 0xFE54:  // ﹔
    case 0xFE55:  // ﹕
    case 0xFE56:  // ﹖
    case 0xFE57:  // ﹗
    case 0xFF01:  // ！
    case 0xFF05:  // ％
    case 0xFF09:  // ）
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF1A:  // ：
    case 0xFF1B:  // ；
    case 0xFF1F:  // ？
    case 0xFF3D:  // ］
    case 0xFF5D:  // ｝
    case 0xFF5E:  // ～
    case 0xFF60:  // ｠
    case 0xFF61:  // ｡
    case 0xFF63:  // ｣
    case 0xFF64:  // ､
      return true;
    default:
      break;
  }
  // Small hiragana and katakana.
  switch (cp) {
    case 0x3041:
    case 0x3043:
    case 0x3045:
    case 0x3047:
    case 0x3049:
    case 0x3063:
    case 0x3083:
    case 0x3085:
    case 0x3087:
    case 0x308E:
    case 0x3095:
    case 0x3096:
    case 0x30A1:
    case 0x30A3:
    case 0x30A5:
    case 0x30A7:
    case 0x30A9:
    case 0x30C3:
    case 0x30E3:
    case 0x30E5:
    case 0x30E7:
    case 0x30EE:
    case 0x30F5:
    case 0x30F6:
      return true;
    default:
      return cp >= 0x31F0 && cp <= 0x31FF;  // katakana phonetic extensions (small)
  }
}

// Kinsoku: opening brackets and quotes never end a line.
inline bool isLineEndProhibited(const uint32_t cp) {
  switch (cp) {
    case '(':
    case '[':
    case '{':
    case 0x2018:  // ‘
    case 0x201C:  // “
    case 0x3008:  // 〈
    case 0x300A:  // 《
    case 0x300C:  // 「
    case 0x300E:  // 『
    case 0x3010:  // 【
    case 0x3014:  // 〔
    case 0x3016:  // 〖
    case 0x3018:  // 〘
    case 0x301A:  // 〚
    case 0x301D:  // 〝
    case 0xFF04:  // ＄
    case 0xFF08:  // （
    case 0xFF3B:  // ［
    case 0xFF5B:  // ｛
    case 0xFF5F:  // ｟
    case 0xFF62:  // ｢
    case 0xFFE1:  // ￡
    case 0xFFE5:  // ￥
      return true;
    default:
      return false;
  }
}

// Dashes and ellipses that are written doubled (——, ……) and stay together.
inline bool isInseparablePair(const uint32_t left, const uint32_t right) {
  return left == right && (left == 0x2014 || left == 0x2015 || left == 0x2025 || left == 0x2026 || left == 0x22EF);
}

// Whether a line may break between adjacent code points `left` and `right`
// without a space. Spaces are break opportunities on their own and are not
// handled here.
inline bool canBreakBetween(const uint32_t left, const uint32_t right) {
  if (isClusterExtender(right) || left == 0x200D) return false;
  if (isLineEndProhibited(left) || isLineStartProhibited(right)) return false;
  if (isInseparablePair(left, right)) return false;
  return isIdeographic(left) || isIdeographic(right);
}

// Leading paragraph whitespace stripped by the CJK paragraph style: ASCII
// space and tab, NBSP, ideographic space and a byte-order mark.
inline bool isParagraphIndentSpace(const uint32_t cp) {
  return cp == ' ' || cp == '\t' || cp == 0x00A0 || cp == 0x3000 || cp == 0xFEFF;
}

// Whether a book sample is CJK-dominant, so its paragraphs get the CJK style
// (stripped leading whitespace, two-em first-line indent, no blank lines).
inline bool isCjkDominant(const char* text, const size_t length) {
  size_t cjk = 0;
  size_t latin = 0;
  size_t pos = 0;
  while (pos < length) {
    uint32_t cp = 0;
    pos += decodeAt(text, pos, length, cp);
    if (isIdeographic(cp)) {
      cjk++;
    } else if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) {
      latin++;
    }
  }
  // A CJK character carries about as much text as a short Latin word.
  return cjk >= 16 && cjk * 4 >= latin;
}

}  // namespace TxtLineBreak
