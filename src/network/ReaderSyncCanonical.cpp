#include "ReaderSyncCanonical.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(READER_SYNC_HOST_TEST)
// Portable host crypto primitive only: the firmware always uses mbedTLS.
#  if defined(__APPLE__)
#    include <CommonCrypto/CommonDigest.h>
#  else
#    include <openssl/sha.h>
#  endif
#else
#  include <mbedtls/sha1.h>
#endif

#if defined(__has_include)
#  if __has_include(<version>)
#    include <version>
#  endif
#endif
// Which shortest-round-trip float implementation to compile. Both are exact;
// the smoke harness builds the file once per choice and requires the same
// bytes from each. Auto-detection picks std::to_chars (libstdc++ 14 on the
// device); libc++ provides the floating-point overloads without defining
// __cpp_lib_to_chars, so it is detected separately.
#if defined(READER_SYNC_PRINTF_FLOAT)
#  define READER_SYNC_USE_TO_CHARS 0
#elif !defined(READER_SYNC_USE_TO_CHARS)
#  if defined(__cpp_lib_to_chars) || defined(_LIBCPP_VERSION)
#    define READER_SYNC_USE_TO_CHARS 1
#  else
#    define READER_SYNC_USE_TO_CHARS 0
#  endif
#endif
#if READER_SYNC_USE_TO_CHARS
#  include <charconv>
#endif

// This file never takes a number value from ArduinoJson's DOM: its parser keeps
// small decimals in `float` and builds the rest as mantissa * 10^exponent, so
// 0.1 comes back as 0.10000000149011612 and 1e16 as 10000000272564224, while
// Python's json.loads is correctly rounded and keeps int and float apart. The
// only way to reproduce json.dumps() bytes is to canonicalize the original
// number lexemes. The smoke harness still writes its fixtures with ArduinoJson's
// native writer and re-parses them with ArduinoJson, so the bytes under test are
// the real thing.

namespace reader_sync {
namespace {

constexpr size_t kMaxInput = MAX_CANONICAL_INPUT;
constexpr size_t kMaxRecord = MAX_CANONICAL_RECORD;
constexpr unsigned kMaxDepth = static_cast<unsigned>(MAX_CANONICAL_DEPTH);
constexpr size_t kMaxMembers = 96;          // member spans alive at once, all levels
constexpr size_t kMaxIntegerDigits = 4300;  // Python's int->str conversion limit
constexpr size_t kUnitsDevice = 128;
constexpr size_t kUnitsPath = 1024;
constexpr size_t kUnitsAnchor = 2048;
constexpr size_t kUnitsText = 8192;
constexpr char kAnchorFormat[] = "reader-workbench-native-v1";
constexpr char kAnnotationFrame[] = "{\"annotations\":[]}";
constexpr uint64_t kMaxSafeInteger = 9007199254740991ull;  // 2^53 - 1
// createdAt must be a safe integer and createdAt * 1000 must stay one too.
constexpr int64_t kMaxCreatedSeconds = static_cast<int64_t>(kMaxSafeInteger / 1000);

const uint8_t kUuidNamespace[16] = {0xc5, 0xa4, 0x68, 0x3a, 0xf8, 0x9f, 0x54, 0xd3,
                                    0x9b, 0x39, 0x4a, 0x6d, 0xb1, 0xda, 0x19, 0x4d};

struct Span {
  uint32_t offset = 0;
  uint32_t length = 0;
};

struct Member {
  Span key;
  Span value;
};

struct Ctx {
  const uint8_t* buf;
  size_t size;
  size_t pos;
  Member* members;
  size_t used;
  const char* error;
};

// ---------------------------------------------------------------------------
// Output sink: a std::string that never grows past the cloud limit.
// ---------------------------------------------------------------------------
class Out {
 public:
  Out(std::string& target, size_t limit) : s_(target), limit_(limit) { s_.clear(); }

  void put(char c) {
    if (s_.size() < limit_) {
      s_.push_back(c);
    } else {
      ok_ = false;
    }
  }
  void put(const char* p, size_t n) {
    if (n > limit_ - s_.size()) {
      ok_ = false;
      return;
    }
    s_.append(p, n);
  }
  void putLiteral(const char* lit) { put(lit, std::strlen(lit)); }
  bool ok() const { return ok_; }

 private:
  std::string& s_;
  size_t limit_;
  bool ok_ = true;
};

bool setError(std::string& error, const char* message) {
  error = message ? message : "Native annotation is not valid JSON";
  return false;
}

bool fail(Ctx& c, const char* message) {
  if (!c.error) c.error = message;
  return false;
}

// ---------------------------------------------------------------------------
// Unicode: Python str semantics (code points), not bytes.
// ---------------------------------------------------------------------------
bool pythonSpace(uint32_t c) {
  return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x1f) || c == 0x20 || c == 0x85 ||
         c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 ||
         c == 0x202f || c == 0x205f || c == 0x3000;
}

size_t encodeUtf8(uint32_t cp, char out[4]) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xc0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3f));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xe0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    out[2] = static_cast<char>(0x80 | (cp & 0x3f));
    return 3;
  }
  out[0] = static_cast<char>(0xf0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
  out[3] = static_cast<char>(0x80 | (cp & 0x3f));
  return 4;
}

// json.dumps(..., ensure_ascii=False): only quote, backslash and the C0 range are
// escaped; DEL and all non-ASCII text stay raw UTF-8.
void putEscaped(Out& out, uint32_t cp) {
  switch (cp) {
    case '"': out.putLiteral("\\\""); return;
    case '\\': out.putLiteral("\\\\"); return;
    case 0x08: out.putLiteral("\\b"); return;
    case 0x0c: out.putLiteral("\\f"); return;
    case 0x0a: out.putLiteral("\\n"); return;
    case 0x0d: out.putLiteral("\\r"); return;
    case 0x09: out.putLiteral("\\t"); return;
    default: break;
  }
  if (cp < 0x20) {
    static constexpr char HEX[] = "0123456789abcdef";
    const char buf[6] = {'\\', 'u', '0', '0', HEX[(cp >> 4) & 0xf], HEX[cp & 0xf]};
    out.put(buf, sizeof(buf));
    return;
  }
  char buf[4];
  out.put(buf, encodeUtf8(cp, buf));
}

void putBytes(Out& out, const char* p, size_t n) {
  out.put('"');
  for (size_t i = 0; i < n; ++i) {
    const uint8_t ch = static_cast<uint8_t>(p[i]);
    if (ch < 0x20 || ch == '"' || ch == '\\') {
      putEscaped(out, ch);
    } else {
      out.put(static_cast<char>(ch));
    }
  }
  out.put('"');
}

void putInt(Out& out, int64_t value) {
  char buf[24];
  size_t n = 0;
  uint64_t magnitude = value < 0 ? uint64_t(0) - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
  if (value < 0) buf[n++] = '-';
  const size_t start = n;
  do {
    buf[n++] = static_cast<char>('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude != 0);
  for (size_t i = 0; i < (n - start) / 2; ++i) {
    const char tmp = buf[start + i];
    buf[start + i] = buf[n - 1 - i];
    buf[n - 1 - i] = tmp;
  }
  out.put(buf, n);
}

// Decodes the code points of a JSON string body: raw UTF-8 plus the escapes
// json.loads accepts, with surrogate pairs combined. Lone surrogates, overlong
// forms and truncated sequences fail, exactly like .encode("utf-8") does.
class Cursor {
 public:
  Cursor(const uint8_t* base, Span body) : p_(base + body.offset), end_(base + body.offset + body.length) {}

  bool next(uint32_t& cp) {
    if (p_ >= end_) return false;
    const uint8_t c = *p_++;
    if (c != '\\') {
      if (c < 0x20) {
        bad_ = true;
        return false;
      }
      if (c < 0x80) {
        cp = c;
        return true;
      }
      --p_;
      return decodeRaw(cp);
    }
    if (p_ >= end_) {
      bad_ = true;
      return false;
    }
    switch (*p_++) {
      case '"': cp = '"'; return true;
      case '\\': cp = '\\'; return true;
      case '/': cp = '/'; return true;
      case 'b': cp = 0x08; return true;
      case 'f': cp = 0x0c; return true;
      case 'n': cp = 0x0a; return true;
      case 'r': cp = 0x0d; return true;
      case 't': cp = 0x09; return true;
      case 'u': return decodeEscape(cp);
      default: bad_ = true; return false;
    }
  }

  bool bad() const { return bad_; }

 private:
  bool readHex4(uint32_t& unit) {
    if (end_ - p_ < 4) {
      bad_ = true;
      return false;
    }
    unit = 0;
    for (int i = 0; i < 4; ++i) {
      const uint8_t c = *p_++;
      uint8_t nibble;
      if (c >= '0' && c <= '9') {
        nibble = static_cast<uint8_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        nibble = static_cast<uint8_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        nibble = static_cast<uint8_t>(c - 'A' + 10);
      } else {
        bad_ = true;
        return false;
      }
      unit = (unit << 4) | nibble;
    }
    return true;
  }

  bool decodeEscape(uint32_t& cp) {
    uint32_t unit = 0;
    if (!readHex4(unit)) return false;
    if (unit >= 0xdc00 && unit <= 0xdfff) {  // lone low surrogate
      bad_ = true;
      return false;
    }
    if (unit < 0xd800 || unit > 0xdbff) {
      cp = unit;
      return true;
    }
    if (end_ - p_ < 6 || p_[0] != '\\' || p_[1] != 'u') {
      bad_ = true;
      return false;
    }
    p_ += 2;
    uint32_t low = 0;
    if (!readHex4(low) || low < 0xdc00 || low > 0xdfff) {
      bad_ = true;
      return false;
    }
    cp = 0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00);
    return true;
  }

  bool decodeRaw(uint32_t& cp) {
    const uint8_t lead = *p_++;
    int extra;
    uint32_t value;
    uint32_t minimum;
    if (lead >= 0xc2 && lead <= 0xdf) {
      extra = 1;
      value = lead & 0x1f;
      minimum = 0x80;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      extra = 2;
      value = lead & 0x0f;
      minimum = 0x800;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      extra = 3;
      value = lead & 0x07;
      minimum = 0x10000;
    } else {
      bad_ = true;
      return false;
    }
    if (end_ - p_ < extra) {
      bad_ = true;
      return false;
    }
    for (int i = 0; i < extra; ++i) {
      const uint8_t c = *p_++;
      if ((c & 0xc0) != 0x80) {
        bad_ = true;
        return false;
      }
      value = (value << 6) | (c & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
      bad_ = true;
      return false;
    }
    cp = value;
    return true;
  }

  const uint8_t* p_;
  const uint8_t* end_;
  bool bad_ = false;
};

Span stringBody(Span span) {
  return Span{static_cast<uint32_t>(span.offset + 1), static_cast<uint32_t>(span.length - 2)};
}

bool isString(Ctx& c, Span span) { return span.length >= 2 && c.buf[span.offset] == '"'; }

// Python sorts str by code point, so keys spelled with escapes ("\u0061") must
// compare as the decoded text rather than as UTF-8 bytes of the escape.
int compareKeys(const uint8_t* base, Span a, Span b) {
  Cursor ca(base, stringBody(a));
  Cursor cb(base, stringBody(b));
  for (;;) {
    uint32_t x = 0;
    uint32_t y = 0;
    const bool hasA = ca.next(x);
    const bool hasB = cb.next(y);
    if (!hasA && !hasB) return 0;
    if (!hasA) return -1;
    if (!hasB) return 1;
    if (x != y) return x < y ? -1 : 1;
  }
}

bool keyIs(const uint8_t* base, Span key, const char* name) {
  Cursor cursor(base, stringBody(key));
  size_t i = 0;
  uint32_t cp;
  while (cursor.next(cp)) {
    if (name[i] == '\0' || static_cast<uint32_t>(static_cast<uint8_t>(name[i])) != cp) return false;
    ++i;
  }
  if (cursor.bad()) return false;
  return name[i] == '\0';
}

// ---------------------------------------------------------------------------
// Parser: strict JSON, bounded depth and field count, spans into the buffer.
// ---------------------------------------------------------------------------
void skipSpace(Ctx& c) {
  while (c.pos < c.size) {
    const uint8_t ch = c.buf[c.pos];
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      ++c.pos;
    } else {
      break;
    }
  }
}

bool isDigit(uint8_t ch) { return ch >= '0' && ch <= '9'; }

bool parseString(Ctx& c, Span& out) {
  if (c.pos >= c.size || c.buf[c.pos] != '"') return fail(c, "JSON string expected");
  const size_t open = c.pos;
  size_t i = c.pos + 1;
  for (;;) {
    if (i >= c.size) return fail(c, "JSON string is unterminated");
    const uint8_t ch = c.buf[i];
    if (ch == '"') break;
    if (ch == '\\') {
      if (i + 1 >= c.size) return fail(c, "JSON string is unterminated");
      i += 2;
      continue;
    }
    ++i;
  }
  const Span body{static_cast<uint32_t>(open + 1), static_cast<uint32_t>(i - open - 1)};
  Cursor cursor(c.buf, body);
  uint32_t cp;
  while (cursor.next(cp)) {
  }
  if (cursor.bad()) return fail(c, "JSON string is not valid UTF-8 or uses an unsupported escape");
  c.pos = i + 1;
  out = Span{static_cast<uint32_t>(open), static_cast<uint32_t>(i + 1 - open)};
  return true;
}

bool parseNumber(Ctx& c, Span& out) {
  const size_t start = c.pos;
  size_t i = c.pos;
  if (i < c.size && c.buf[i] == '-') ++i;
  if (i >= c.size) return fail(c, "Invalid JSON number");
  if (c.buf[i] == '0') {
    ++i;
  } else if (c.buf[i] >= '1' && c.buf[i] <= '9') {
    while (i < c.size && isDigit(c.buf[i])) ++i;
  } else {
    return fail(c, "Invalid JSON number");
  }
  if (i < c.size && c.buf[i] == '.') {
    ++i;
    if (i >= c.size || !isDigit(c.buf[i])) return fail(c, "Invalid JSON number");
    while (i < c.size && isDigit(c.buf[i])) ++i;
  }
  if (i < c.size && (c.buf[i] == 'e' || c.buf[i] == 'E')) {
    ++i;
    if (i < c.size && (c.buf[i] == '+' || c.buf[i] == '-')) ++i;
    if (i >= c.size || !isDigit(c.buf[i])) return fail(c, "Invalid JSON number");
    while (i < c.size && isDigit(c.buf[i])) ++i;
  }
  c.pos = i;
  out = Span{static_cast<uint32_t>(start), static_cast<uint32_t>(i - start)};
  return true;
}

bool parseLiteral(Ctx& c, const char* literal, Span& out) {
  const size_t n = std::strlen(literal);
  if (c.size - c.pos < n || std::memcmp(c.buf + c.pos, literal, n) != 0) {
    return fail(c, "Invalid JSON literal");
  }
  out = Span{static_cast<uint32_t>(c.pos), static_cast<uint32_t>(n)};
  c.pos += n;
  return true;
}

bool parseValue(Ctx& c, Span& out, unsigned depth);

bool parseArray(Ctx& c, Span& out, unsigned depth) {
  const size_t begin = c.pos;
  ++c.pos;
  skipSpace(c);
  if (c.pos < c.size && c.buf[c.pos] == ']') {
    ++c.pos;
    out = Span{static_cast<uint32_t>(begin), static_cast<uint32_t>(c.pos - begin)};
    return true;
  }
  for (;;) {
    Span element;
    const size_t mark = c.used;
    if (!parseValue(c, element, depth + 1)) return false;
    c.used = mark;
    skipSpace(c);
    if (c.pos >= c.size) return fail(c, "JSON array is unterminated");
    if (c.buf[c.pos] == ',') {
      ++c.pos;
      continue;
    }
    if (c.buf[c.pos] == ']') {
      ++c.pos;
      break;
    }
    return fail(c, "Invalid JSON array separator");
  }
  out = Span{static_cast<uint32_t>(begin), static_cast<uint32_t>(c.pos - begin)};
  return true;
}

bool parseObject(Ctx& c, Span& out, unsigned depth, size_t& base, size_t& count) {
  const size_t begin = c.pos;
  ++c.pos;
  const size_t entryBase = c.used;
  skipSpace(c);
  if (c.pos < c.size && c.buf[c.pos] == '}') {
    ++c.pos;
    out = Span{static_cast<uint32_t>(begin), static_cast<uint32_t>(c.pos - begin)};
    base = entryBase;
    count = 0;
    return true;
  }
  for (;;) {
    skipSpace(c);
    Span key;
    if (!parseString(c, key)) return false;
    for (size_t i = entryBase; i < c.used; ++i) {
      if (compareKeys(c.buf, c.members[i].key, key) == 0) {
        return fail(c, "Native annotation repeats a field name");
      }
    }
    skipSpace(c);
    if (c.pos >= c.size || c.buf[c.pos] != ':') return fail(c, "JSON object key requires ':'");
    ++c.pos;
    Span value;
    const size_t mark = c.used;
    if (!parseValue(c, value, depth + 1)) return false;
    c.used = mark;
    if (c.used >= kMaxMembers) return fail(c, "Native annotation has too many fields to preserve exactly");
    c.members[c.used].key = key;
    c.members[c.used].value = value;
    ++c.used;
    skipSpace(c);
    if (c.pos >= c.size) return fail(c, "JSON object is unterminated");
    if (c.buf[c.pos] == ',') {
      ++c.pos;
      continue;
    }
    if (c.buf[c.pos] == '}') {
      ++c.pos;
      break;
    }
    return fail(c, "Invalid JSON object separator");
  }
  base = entryBase;
  count = c.used - entryBase;
  out = Span{static_cast<uint32_t>(begin), static_cast<uint32_t>(c.pos - begin)};
  return true;
}

bool parseValue(Ctx& c, Span& out, unsigned depth) {
  if (depth > kMaxDepth) return fail(c, "Native annotation nests deeper than 10 levels");
  skipSpace(c);
  if (c.pos >= c.size) return fail(c, "JSON value expected");
  const uint8_t ch = c.buf[c.pos];
  if (ch == '{') {
    size_t base = 0;
    size_t count = 0;
    return parseObject(c, out, depth, base, count);
  }
  if (ch == '[') return parseArray(c, out, depth);
  if (ch == '"') return parseString(c, out);
  if (ch == 't') return parseLiteral(c, "true", out);
  if (ch == 'f') return parseLiteral(c, "false", out);
  if (ch == 'n') return parseLiteral(c, "null", out);
  if (ch == '-' || isDigit(ch)) return parseNumber(c, out);
  return fail(c, "Invalid JSON value");
}

bool memberSpan(Ctx& c, size_t base, size_t count, const char* name, Span& out) {
  for (size_t i = base; i < base + count; ++i) {
    if (keyIs(c.buf, c.members[i].key, name)) {
      out = c.members[i].value;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Python float repr.
// ---------------------------------------------------------------------------
bool parseScientific(const char* text, char digits[24], int& count, int& decpt) {
  int i = 0;
  if (text[i] == '+' || text[i] == '-') ++i;
  count = 0;
  while (isDigit(static_cast<uint8_t>(text[i]))) {
    if (count >= 23) return false;
    digits[count++] = text[i++];
  }
  if (text[i] == '.') {
    ++i;
    while (isDigit(static_cast<uint8_t>(text[i]))) {
      if (count >= 23) return false;
      digits[count++] = text[i++];
    }
  }
  if (count == 0) return false;
  int exponent = 0;
  if (text[i] == 'e' || text[i] == 'E') {
    ++i;
    int sign = 1;
    if (text[i] == '+') {
      ++i;
    } else if (text[i] == '-') {
      sign = -1;
      ++i;
    }
    if (!isDigit(static_cast<uint8_t>(text[i]))) return false;
    while (isDigit(static_cast<uint8_t>(text[i]))) {
      if (exponent < 100000) exponent = exponent * 10 + (text[i] - '0');
      ++i;
    }
    exponent *= sign;
  } else {
    return false;
  }
  if (text[i] != '\0') return false;
  while (count > 1 && digits[count - 1] == '0') --count;
  decpt = exponent + 1;
  return true;
}

// Shortest round-tripping decimal digits of a finite positive value, as
// value = 0.DIGITS * 10^decpt (decpt = digits before the point).
bool shortestDigits(double value, char digits[24], int& count, int& decpt) {
#if READER_SYNC_USE_TO_CHARS
  char buf[40];
  const auto result = std::to_chars(buf, buf + sizeof(buf) - 1, value, std::chars_format::scientific);
  if (result.ec != std::errc()) return false;
  *result.ptr = '\0';
  return parseScientific(buf, digits, count, decpt);
#else
  // printf("%.*e") and strtod are both correctly rounded, so the smallest
  // precision whose output reads back as the same double is exactly Python's
  // shortest repr. The predicate is monotone, so search it.
  char buf[40];
  int low = 1;
  int high = 17;
  while (low < high) {
    const int mid = (low + high) / 2;
    std::snprintf(buf, sizeof(buf), "%.*e", mid - 1, value);
    if (std::strtod(buf, nullptr) == value) {
      high = mid;
    } else {
      low = mid + 1;
    }
  }
  std::snprintf(buf, sizeof(buf), "%.*e", low - 1, value);
  return parseScientific(buf, digits, count, decpt);
#endif
}

// float.__repr__: fixed notation while -4 < decpt <= 16, scientific otherwise,
// integer-valued floats keep ".0", negative zero keeps its sign.
bool putDouble(Out& out, double value) {
  const bool negative = std::signbit(value);
  if (value == 0) {
    out.putLiteral(negative ? "-0.0" : "0.0");
    return out.ok();
  }
  char digits[24];
  int count = 0;
  int decpt = 0;
  if (!shortestDigits(negative ? -value : value, digits, count, decpt)) return false;
  char buf[40];
  size_t n = 0;
  if (negative) buf[n++] = '-';
  if (decpt <= -4 || decpt > 16) {
    buf[n++] = digits[0];
    if (count > 1) {
      buf[n++] = '.';
      for (int i = 1; i < count; ++i) buf[n++] = digits[i];
    }
    buf[n++] = 'e';
    int exponent = decpt - 1;
    if (exponent < 0) {
      buf[n++] = '-';
      exponent = -exponent;
    } else {
      buf[n++] = '+';
    }
    char reversed[4];
    int length = 0;
    do {
      reversed[length++] = static_cast<char>('0' + exponent % 10);
      exponent /= 10;
    } while (exponent != 0 && length < 4);
    while (length < 2) reversed[length++] = '0';
    while (length > 0) buf[n++] = reversed[--length];
  } else if (decpt <= 0) {
    buf[n++] = '0';
    buf[n++] = '.';
    for (int i = 0; i < -decpt; ++i) buf[n++] = '0';
    for (int i = 0; i < count; ++i) buf[n++] = digits[i];
  } else if (decpt >= count) {
    for (int i = 0; i < count; ++i) buf[n++] = digits[i];
    for (int i = count; i < decpt; ++i) buf[n++] = '0';
    buf[n++] = '.';
    buf[n++] = '0';
  } else {
    for (int i = 0; i < decpt; ++i) buf[n++] = digits[i];
    buf[n++] = '.';
    for (int i = decpt; i < count; ++i) buf[n++] = digits[i];
  }
  out.put(buf, n);
  return out.ok();
}

// ---------------------------------------------------------------------------
// Canonical writer over parsed spans.
// ---------------------------------------------------------------------------
bool emitValue(Ctx& c, Span span, Out& out, unsigned depth);

bool emitNumber(Ctx& c, Span span, Out& out) {
  const char* text = reinterpret_cast<const char*>(c.buf + span.offset);
  const size_t length = span.length;
  bool fractional = false;
  size_t digitCount = 0;
  for (size_t i = 0; i < length; ++i) {
    const char ch = text[i];
    if (ch == '.' || ch == 'e' || ch == 'E') {
      fractional = true;
    } else if (ch >= '0' && ch <= '9') {
      ++digitCount;
    }
  }
  if (!fractional) {
    if (digitCount > kMaxIntegerDigits) {
      return fail(c, "Native annotation integer exceeds Python's 4300-digit limit");
    }
    size_t start = 0;
    bool negative = false;
    if (text[0] == '-') {
      negative = true;
      start = 1;
    }
    bool zero = true;
    for (size_t i = start; i < length; ++i) {
      if (text[i] != '0') {
        zero = false;
        break;
      }
    }
    if (zero) {
      out.put('0');  // json.loads("-0") is int 0
      return out.ok();
    }
    if (negative) out.put('-');
    out.put(text + start, length - start);
    return out.ok();
  }
  char small[64];
  double value;
  if (length < sizeof(small)) {
    std::memcpy(small, text, length);
    small[length] = '\0';
    value = std::strtod(small, nullptr);
  } else {
    const std::string token(text, length);
    value = std::strtod(token.c_str(), nullptr);
  }
  if (!std::isfinite(value)) {
    return fail(c, "Native annotation contains a number json.dumps cannot serialize");
  }
  if (!putDouble(out, value)) return fail(c, "Native annotation number cannot be formatted exactly");
  return out.ok();
}

bool emitMembers(Ctx& c, size_t base, size_t count, Out& out, const char* skipA, const char* skipB,
                 unsigned depth) {
  bool first = true;
  Span last{};
  bool hasLast = false;
  for (size_t emitted = 0; emitted < count; ++emitted) {
    size_t best = count;
    for (size_t i = 0; i < count; ++i) {
      const Span key = c.members[base + i].key;
      if ((skipA && keyIs(c.buf, key, skipA)) || (skipB && keyIs(c.buf, key, skipB))) continue;
      if (hasLast && compareKeys(c.buf, key, last) <= 0) continue;
      if (best == count || compareKeys(c.buf, key, c.members[base + best].key) < 0) best = i;
    }
    if (best == count) break;
    last = c.members[base + best].key;
    hasLast = true;
    if (!first) out.put(',');
    first = false;
    Cursor cursor(c.buf, stringBody(last));
    out.put('"');
    uint32_t cp;
    while (cursor.next(cp)) putEscaped(out, cp);
    if (cursor.bad()) return fail(c, "Native annotation field name is not valid UTF-8");
    out.put('"');
    out.put(':');
    if (!emitValue(c, c.members[base + best].value, out, depth + 1)) return false;
  }
  return out.ok();
}

bool emitValue(Ctx& c, Span span, Out& out, unsigned depth) {
  if (depth > kMaxDepth) return fail(c, "Native annotation nests deeper than 10 levels");
  const uint8_t kind = c.buf[span.offset];
  if (kind == '"') {
    Cursor cursor(c.buf, stringBody(span));
    out.put('"');
    uint32_t cp;
    while (cursor.next(cp)) putEscaped(out, cp);
    if (cursor.bad()) return fail(c, "Native annotation string is not valid UTF-8");
    out.put('"');
    return out.ok();
  }
  if (kind == '{') {
    const size_t mark = c.used;
    c.pos = span.offset;
    Span parsed;
    size_t base = 0;
    size_t count = 0;
    if (!parseObject(c, parsed, depth, base, count)) return false;
    out.put('{');
    const bool ok = emitMembers(c, base, count, out, nullptr, nullptr, depth);
    c.used = mark;
    out.put('}');
    return ok && out.ok();
  }
  if (kind == '[') {
    c.pos = span.offset + 1;
    skipSpace(c);
    out.put('[');
    if (c.pos < c.size && c.buf[c.pos] == ']') {
      out.put(']');
      return out.ok();
    }
    bool first = true;
    for (;;) {
      if (!first) out.put(',');
      first = false;
      Span element;
      const size_t mark = c.used;
      if (!parseValue(c, element, depth + 1)) return false;
      c.used = mark;
      if (!emitValue(c, element, out, depth + 1)) return false;
      skipSpace(c);
      if (c.pos >= c.size) return fail(c, "JSON array is unterminated");
      if (c.buf[c.pos] == ',') {
        ++c.pos;
        continue;
      }
      if (c.buf[c.pos] == ']') {
        ++c.pos;
        break;
      }
      return fail(c, "Invalid JSON array separator");
    }
    out.put(']');
    return out.ok();
  }
  if (kind == 't') {
    out.putLiteral("true");
    return out.ok();
  }
  if (kind == 'f') {
    out.putLiteral("false");
    return out.ok();
  }
  if (kind == 'n') {
    out.putLiteral("null");
    return out.ok();
  }
  return emitNumber(c, span, out);
}

bool emitStringSpan(Ctx& c, Span span, Out& out) {
  if (!isString(c, span)) return fail(c, "JSON string expected");
  return emitValue(c, span, out, 0);
}

// ---------------------------------------------------------------------------
// Cloud text limits: JavaScript UTF-16 units, control characters, blank text.
// ---------------------------------------------------------------------------
struct TextCheck {
  size_t units = 0;
  bool blank = true;
  bool multiline;
  explicit TextCheck(bool multi) : multiline(multi) {}

  bool add(uint32_t cp) {
    if (cp < 0x20) {
      if (!(multiline && (cp == '\r' || cp == '\n' || cp == '\t'))) return false;
    } else if (cp == 0x7f) {
      return false;
    }
    if (!pythonSpace(cp)) blank = false;
    units += cp > 0xffff ? 2 : 1;
    return true;
  }
};

bool finishText(const TextCheck& check, size_t maximum, bool allowEmpty, const char* field,
                std::string& error) {
  if (check.units > maximum || (!allowEmpty && check.blank)) {
    error = std::string(field) + " is empty, contains control characters, or exceeds " +
            std::to_string(maximum) + " UTF-16 units; nothing is truncated";
    return false;
  }
  return true;
}

bool textSpan(Ctx& c, Span span, size_t maximum, bool allowEmpty, bool multiline, const char* field,
              std::string& error) {
  if (!isString(c, span)) {
    error = std::string(field) + " must be text";
    return false;
  }
  Cursor cursor(c.buf, stringBody(span));
  TextCheck check(multiline);
  uint32_t cp;
  while (cursor.next(cp)) {
    if (!check.add(cp)) {
      error = std::string(field) + " contains control characters; nothing is truncated";
      return false;
    }
  }
  if (cursor.bad()) {
    error = std::string(field) + " contains invalid Unicode";
    return false;
  }
  return finishText(check, maximum, allowEmpty, field, error);
}

// Raw UTF-8 text (device id, already-serialized anchor): no escapes to decode.
bool textBytes(const char* text, size_t length, size_t maximum, bool allowEmpty, bool multiline,
               const char* field, std::string& error) {
  Cursor cursor(reinterpret_cast<const uint8_t*>(text), Span{0, static_cast<uint32_t>(length)});
  TextCheck check(multiline);
  uint32_t cp;
  while (cursor.next(cp)) {
    if (!check.add(cp)) {
      error = std::string(field) + " contains control characters; nothing is truncated";
      return false;
    }
  }
  if (cursor.bad()) {
    error = std::string(field) + " contains invalid Unicode";
    return false;
  }
  return finishText(check, maximum, allowEmpty, field, error);
}

// ---------------------------------------------------------------------------
// Integers and kind names.
// ---------------------------------------------------------------------------
bool integerSpan(Ctx& c, Span span, int64_t minimum, int64_t maximum, int64_t& out) {
  const char* text = reinterpret_cast<const char*>(c.buf + span.offset);
  const size_t length = span.length;
  size_t i = 0;
  bool negative = false;
  if (length > 0 && text[0] == '-') {
    negative = true;
    i = 1;
  }
  if (i >= length) return false;
  constexpr uint64_t kLimit = uint64_t(1) << 63;
  uint64_t magnitude = 0;
  for (; i < length; ++i) {
    const char ch = text[i];
    if (ch < '0' || ch > '9') return false;  // fractions and exponents are not ints
    if (magnitude > kLimit / 10) return false;
    magnitude = magnitude * 10 + static_cast<uint64_t>(ch - '0');
    if (magnitude > kLimit) return false;
  }
  int64_t value;
  if (negative) {
    value = magnitude == kLimit ? INT64_MIN : -static_cast<int64_t>(magnitude);
  } else {
    if (magnitude >= kLimit) return false;
    value = static_cast<int64_t>(magnitude);
  }
  if (value < minimum || value > maximum) return false;
  out = value;
  return true;
}

bool kindFromString(Ctx& c, Span span, const char*& name) {
  char buffer[32];
  Cursor cursor(c.buf, stringBody(span));
  size_t length = 0;
  uint32_t cp;
  while (cursor.next(cp)) {
    if (cp > 0x7e || length >= sizeof(buffer) - 1) return false;
    buffer[length++] = static_cast<char>(cp);
  }
  if (cursor.bad()) return false;
  buffer[length] = '\0';
  const char* text = buffer;
  constexpr char PREFIX[] = "AnnotationKind::";
  constexpr size_t PREFIX_LENGTH = sizeof(PREFIX) - 1;
  if (length >= PREFIX_LENGTH && std::memcmp(text, PREFIX, PREFIX_LENGTH) == 0) {
    text += PREFIX_LENGTH;
  }
  if (std::strcmp(text, "Quote") == 0 || std::strcmp(text, "quote") == 0) {
    name = "quote";
    return true;
  }
  if (std::strcmp(text, "Question") == 0 || std::strcmp(text, "question") == 0) {
    name = "question";
    return true;
  }
  if (std::strcmp(text, "ReviewLater") == 0 || std::strcmp(text, "review_later") == 0) {
    name = "review_later";
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// SHA-1: mbedTLS on device, a host primitive only for the harness.
// ---------------------------------------------------------------------------
#if defined(READER_SYNC_HOST_TEST)
#  if defined(__APPLE__)
using Sha1Ctx = CC_SHA1_CTX;
inline void sha1Init(Sha1Ctx& ctx) { CC_SHA1_Init(&ctx); }
inline bool sha1Update(Sha1Ctx& ctx, const void* data, size_t size) {
  CC_SHA1_Update(&ctx, data, static_cast<CC_LONG>(size));
  return true;
}
inline bool sha1Finish(Sha1Ctx& ctx, uint8_t digest[20]) {
  CC_SHA1_Final(digest, &ctx);
  return true;
}
#  else
using Sha1Ctx = SHA_CTX;
inline void sha1Init(Sha1Ctx& ctx) { SHA1_Init(&ctx); }
inline bool sha1Update(Sha1Ctx& ctx, const void* data, size_t size) {
  SHA1_Update(&ctx, data, size);
  return true;
}
inline bool sha1Finish(Sha1Ctx& ctx, uint8_t digest[20]) {
  SHA1_Final(digest, &ctx);
  return true;
}
#  endif
#else
using Sha1Ctx = mbedtls_sha1_context;
inline void sha1Init(Sha1Ctx& ctx) { mbedtls_sha1_init(&ctx); }
inline bool sha1Start(Sha1Ctx& ctx) { return mbedtls_sha1_starts(&ctx) == 0; }
inline bool sha1Update(Sha1Ctx& ctx, const void* data, size_t size) {
  return mbedtls_sha1_update(&ctx, static_cast<const unsigned char*>(data), size) == 0;
}
inline bool sha1Finish(Sha1Ctx& ctx, uint8_t digest[20]) {
  const bool ok = mbedtls_sha1_finish(&ctx, digest) == 0;
  mbedtls_sha1_free(&ctx);
  return ok;
}
#endif

bool uuid5(const std::string& stable, char out[37]) {
  uint8_t digest[20];
  Sha1Ctx ctx;
  sha1Init(ctx);
#if !defined(READER_SYNC_HOST_TEST)
  if (!sha1Start(ctx)) return false;
#endif
  if (!sha1Update(ctx, kUuidNamespace, sizeof(kUuidNamespace))) return false;
  if (!sha1Update(ctx, stable.data(), stable.size())) return false;
  if (!sha1Finish(ctx, digest)) return false;
  // uuid.uuid5: first 16 bytes with the version-5 and RFC 4122 variant bits.
  digest[6] = static_cast<uint8_t>((digest[6] & 0x0f) | 0x50);
  digest[8] = static_cast<uint8_t>((digest[8] & 0x3f) | 0x80);
  static constexpr char HEX[] = "0123456789abcdef";
  size_t n = 0;
  for (size_t i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out[n++] = '-';
    out[n++] = HEX[digest[i] >> 4];
    out[n++] = HEX[digest[i] & 0x0f];
  }
  out[n] = '\0';
  return n == 36;
}

}  // namespace

bool canonicalAnnotation(const char* raw, size_t length, const char* deviceId, int64_t nowSeconds,
                         std::string& uploadRecord, std::string& uuid, std::string& error) {
  uploadRecord.clear();
  uuid.clear();
  error.clear();

  if (!raw) return setError(error, "No native annotation record supplied");
  if (length == 0) return setError(error, "Native annotation record is empty");
  if (length > kMaxInput) return setError(error, "Native annotation exceeds 8192 bytes; nothing is truncated");
  const char* device = deviceId ? deviceId : "";
  const size_t deviceLength = std::strlen(device);

  Member members[kMaxMembers];
  Ctx c{reinterpret_cast<const uint8_t*>(raw), length, 0, members, 0, nullptr};
  size_t base = 0;
  size_t count = 0;
  Span root;
  skipSpace(c);
  if (c.pos >= c.size || c.buf[c.pos] != '{') return setError(error, "Each annotation must be a JSON object");
  if (!parseObject(c, root, 0, base, count)) return setError(error, c.error);
  skipSpace(c);
  if (c.pos != c.size) return setError(error, "Native annotation has trailing data after the JSON object");

  if (!textBytes(device, deviceLength, kUnitsDevice, false, false, "Cloud device_id", error)) return false;

  Span sourcePath;
  if (!memberSpan(c, base, count, "sourcePath", sourcePath)) {
    return setError(error,
                    "Native annotation is missing sourcePath; only firmware-native records are uploaded here");
  }
  if (!textSpan(c, sourcePath, kUnitsPath, false, false, "Cloud book_path", error)) return false;

  Span idSpan;
  int64_t nativeId = 0;
  if (!memberSpan(c, base, count, "id", idSpan) || !integerSpan(c, idSpan, 1, 128, nativeId)) {
    return setError(error, "Native annotation id must be an integer from 1 to 128");
  }
  (void)nativeId;

  Span createdSpan;
  int64_t createdSeconds = 0;
  if (!memberSpan(c, base, count, "createdAt", createdSpan) ||
      !integerSpan(c, createdSpan, 0, kMaxCreatedSeconds, createdSeconds)) {
    return setError(error,
                    "Native createdAt must be an integer UTC second count no larger than 9007199254740");
  }
  const int64_t capturedMillis = createdSeconds * 1000;

  const char* kindName = "quote";
  Span kindSpan;
  if (memberSpan(c, base, count, "kind", kindSpan)) {
    if (isString(c, kindSpan)) {
      if (!kindFromString(c, kindSpan, kindName)) {
        return setError(error, "Native kind must be 0/1/2 or Quote/Question/ReviewLater");
      }
    } else {
      int64_t kindValue = 0;
      if (!integerSpan(c, kindSpan, 0, 2, kindValue)) {
        return setError(error, "Native kind must be 0/1/2 or Quote/Question/ReviewLater");
      }
      static constexpr const char* NAMES[3] = {"quote", "question", "review_later"};
      kindName = NAMES[kindValue];
    }
  }

  Span excerptSpan;
  const bool hasExcerpt = memberSpan(c, base, count, "excerpt", excerptSpan);
  if (hasExcerpt && !textSpan(c, excerptSpan, kUnitsText, true, true, "Cloud selection", error)) return false;
  Span noteSpan;
  const bool hasNote = memberSpan(c, base, count, "note", noteSpan);
  if (hasNote && !textSpan(c, noteSpan, kUnitsText, true, true, "Cloud note", error)) return false;

  // stable = json.dumps([device_id, None, value])
  std::string stable;
  {
    Out out(stable, kMaxRecord);
    out.put('[');
    putBytes(out, device, deviceLength);
    out.putLiteral(",null,");
    out.put('{');
    if (!emitMembers(c, base, count, out, nullptr, nullptr, 1)) {
      return setError(error, c.error ? c.error : "Native annotation cannot be serialized exactly");
    }
    out.put('}');
    out.put(']');
    if (!out.ok()) return setError(error, "Native annotation identity exceeds the 64 KiB cloud limit");
  }

  char uuidText[37];
  if (!uuid5(stable, uuidText)) return setError(error, "UUID computation failed");
  stable = std::string();

  // source_anchor = json.dumps({format, device_id, native}) without excerpt/note
  std::string anchor;
  {
    Out out(anchor, kMaxRecord);
    out.putLiteral("{\"device_id\":");
    putBytes(out, device, deviceLength);
    out.putLiteral(",\"format\":\"");
    out.putLiteral(kAnchorFormat);
    out.putLiteral("\",\"native\":");
    out.put('{');
    if (!emitMembers(c, base, count, out, "excerpt", "note", 1)) {
      return setError(error, c.error ? c.error : "Native annotation cannot be serialized exactly");
    }
    out.put('}');
    out.put('}');
    if (!out.ok()) return setError(error, "Native annotation source_anchor exceeds the 64 KiB cloud limit");
  }
  if (!textBytes(anchor.data(), anchor.size(), kUnitsAnchor, false, false, "Cloud source_anchor", error)) {
    return false;
  }

  // record: sorted {"book_path","created_at","id","item_id","kind","note","selection","source_anchor"}
  {
    Out out(uploadRecord, kMaxRecord);
    out.putLiteral("{\"book_path\":");
    if (!emitStringSpan(c, sourcePath, out)) return setError(error, "Cloud book_path contains invalid Unicode");
    out.putLiteral(",\"created_at\":");
    putInt(out, capturedMillis);
    out.putLiteral(",\"id\":");
    putBytes(out, uuidText, 36);
    out.putLiteral(",\"item_id\":null,\"kind\":");
    putBytes(out, kindName, std::strlen(kindName));
    out.putLiteral(",\"note\":");
    if (hasNote) {
      if (!emitStringSpan(c, noteSpan, out)) return setError(error, "Cloud note contains invalid Unicode");
    } else {
      out.putLiteral("\"\"");
    }
    out.putLiteral(",\"selection\":");
    if (hasExcerpt) {
      if (!emitStringSpan(c, excerptSpan, out)) return setError(error, "Cloud selection contains invalid Unicode");
    } else {
      out.putLiteral("\"\"");
    }
    out.putLiteral(",\"source_anchor\":");
    putBytes(out, anchor.data(), anchor.size());
    out.put('}');
    if (!out.ok() || uploadRecord.size() + std::strlen(kAnnotationFrame) > 64 * 1024) {
      uploadRecord.clear();
      return setError(error, "A single annotation exceeds 64 KiB; use local export, never truncate");
    }
  }

  int64_t latest;
  if (nowSeconds > (INT64_MAX - 300000) / 1000) {
    latest = INT64_MAX;
  } else if (nowSeconds < INT64_MIN / 1000) {
    latest = INT64_MIN;
  } else {
    latest = nowSeconds * 1000 + 300000;
  }
  if (capturedMillis > latest) {
    uploadRecord.clear();
    return setError(
        error, "Annotation capture time is over five minutes in the future; correct its source clock/time before upload");
  }

  uuid.assign(uuidText, 36);
  return true;
}

}  // namespace reader_sync
