#include "ConceptEngine.h"

#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ConceptEngine {
namespace {

// ---- PLHN v1 layout -------------------------------------------------------
constexpr uint32_t PLHN_MAGIC = 0x504C484E;  // "PLHN"
constexpr size_t HEADER_BYTES = 32;
constexpr size_t D_BYTE = 64;
constexpr size_t D_ROUTER = 32;
constexpr size_t SPAN_MAX = 12;
constexpr size_t ENGRAM_DIMS = 24;
constexpr int ACT_SHIFT = 5;  // activations are int8 at 2^-5

constexpr size_t OFF_EMBED = HEADER_BYTES;
constexpr size_t OFF_C1_DW = OFF_EMBED + 256 * D_BYTE;
constexpr size_t OFF_C1_PW = OFF_C1_DW + D_BYTE * 3;
constexpr size_t OFF_C1_GATE = OFF_C1_PW + D_BYTE * D_BYTE;
constexpr size_t OFF_C2_DW = OFF_C1_GATE + D_BYTE * D_BYTE;
constexpr size_t OFF_C2_PW = OFF_C2_DW + D_BYTE * 3;
constexpr size_t OFF_C2_GATE = OFF_C2_PW + D_BYTE * D_BYTE;
constexpr size_t OFF_ROUTER_Q = OFF_C2_GATE + D_BYTE * D_BYTE;
constexpr size_t OFF_ROUTER_K = OFF_ROUTER_Q + D_BYTE * D_ROUTER;
constexpr size_t OFF_CHUNK_PROJ = OFF_ROUTER_K + D_BYTE * D_ROUTER;
constexpr size_t OFF_CORE_DW = OFF_CHUNK_PROJ + D_BYTE * DIM;
constexpr size_t OFF_CORE_PW = OFF_CORE_DW + DIM * 3;
constexpr size_t OFF_CORE_GATE = OFF_CORE_PW + DIM * DIM;
constexpr size_t OFF_CORE_OUT = OFF_CORE_GATE + DIM * DIM;
constexpr size_t MODEL_BYTES = OFF_CORE_OUT + DIM * DIM;
static_assert(MODEL_BYTES == 95008, "PLHN v1 file size");

// ---- ENGR / ICPT ------------------------------------------------------------
constexpr uint32_t ENGR_MAGIC = 0x454E4752;  // "ENGR"
constexpr size_t ENGR_HEADER_BYTES = 512;
constexpr size_t ENGR_SLOT_BYTES = 64;
constexpr size_t ENGR_GLOSS_OFFSET = 8 + ENGRAM_DIMS;
constexpr size_t ENGR_GLOSS_BYTES = 32;

constexpr uint32_t ICPT_MAGIC = 0x49435054;  // "ICPT"
constexpr size_t ICPT_HEADER_BYTES = 16;
constexpr size_t ICPT_RECORD_BYTES = 192;
constexpr size_t ICPT_LABEL_OFFSET = 16;
constexpr size_t ICPT_LABEL_BYTES = 48;
constexpr size_t ICPT_VEC_OFFSET = 64;
constexpr size_t ICPT_BATCH = 16;
// 192 KB on the card; every lookup scans it once to query and once to append.
constexpr uint32_t MAX_EPISODIC_RECORDS = 1024;

constexpr size_t MAX_SCORED_SENSES = 24;

uint32_t readU32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}
uint16_t readU16(const uint8_t* p) {
  uint16_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}
uint64_t readU64(const uint8_t* p) {
  uint64_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}
float readF32(const uint8_t* p) {
  float v;
  memcpy(&v, p, sizeof(v));
  return v;
}

// Header scales are exact powers of two: 2^-S.
bool scaleShift(const float scale, int& shift) {
  if (!(scale > 0.0f) || !std::isfinite(scale)) return false;
  const long s = std::lround(-std::log2(static_cast<double>(scale)));
  if (s < -24 || s > 30) return false;
  shift = static_cast<int>(s);
  return true;
}

int32_t sh(const int32_t a, const int s) {
  if (s > 0) return (a + (1 << (s - 1))) >> s;
  if (s < 0) return a << -s;
  return a;
}
int8_t sat8(const int32_t a) { return static_cast<int8_t>(std::clamp<int32_t>(a, -127, 127)); }
int8_t rq(const int32_t acc, const int s) { return sat8(sh(acc, s)); }

uint64_t isqrt(const uint64_t v) {
  auto r = static_cast<uint64_t>(std::sqrt(static_cast<double>(v)));
  while (r > 0 && r * r > v) --r;
  while ((r + 1) * (r + 1) <= v) ++r;
  return r;
}

int64_t rdiv(const int64_t a, const int64_t n) { return a >= 0 ? (a + n / 2) / n : -((-a + n / 2) / n); }

// Step 8 of the forward pass, shared with the fallback: exact L2 to int8.
void normalize(const int32_t* sums, int8_t out[DIM]) {
  int64_t n2 = 0;
  for (size_t d = 0; d < DIM; ++d) n2 += static_cast<int64_t>(sums[d]) * sums[d];
  const auto n = static_cast<int64_t>(isqrt(static_cast<uint64_t>(n2)));
  for (size_t d = 0; d < DIM; ++d) {
    out[d] = n == 0 ? 0 : static_cast<int8_t>(std::clamp<int64_t>(rdiv(127 * static_cast<int64_t>(sums[d]), n), -127, 127));
  }
}

// ---- Storage paths and model state -----------------------------------------
Paths paths;

struct Model {
  HalMemory::PsramBuffer weights;
  int se = 0, sr = 0, sp = 0, sc = 0;
  // Set once a present file failed to load (bad header, short read, no
  // PSRAM); a missing file is retried, so a model copied over USB is found.
  bool failed = false;
};
Model model;

const int8_t* weightsAt(const size_t offset) { return reinterpret_cast<const int8_t*>(model.weights.get() + offset); }

bool loadModel() {
  if (model.weights) return true;
  if (model.failed || !Storage.exists(paths.model)) return false;
  model.failed = true;
  HalFile file;
  if (!Storage.openFileForRead("CPT", paths.model, file)) return false;
  if (file.fileSize() != MODEL_BYTES) {
    LOG_ERR("CPT", "Model size %u, expected %u", static_cast<unsigned>(file.fileSize()),
            static_cast<unsigned>(MODEL_BYTES));
    return false;
  }
  auto buffer = HalMemory::allocatePsram(MODEL_BYTES);
  if (!buffer) {
    LOG_ERR("CPT", "OOM: model weights");
    return false;
  }
  size_t done = 0;
  while (done < MODEL_BYTES) {
    const int got = file.read(buffer.get() + done, MODEL_BYTES - done);
    if (got <= 0) return false;
    done += static_cast<size_t>(got);
  }
  file.close();
  const uint8_t* h = buffer.get();
  if (readU32(h) != PLHN_MAGIC || readU16(h + 4) != 1 || readU16(h + 6) != D_BYTE || readU16(h + 8) != DIM ||
      h[10] != 2 || h[11] != 2 || h[12] != D_ROUTER || h[13] != 16) {
    LOG_ERR("CPT", "Bad model header");
    return false;
  }
  Model loaded;
  if (!scaleShift(readF32(h + 14), loaded.se) || !scaleShift(readF32(h + 18), loaded.sr) ||
      !scaleShift(readF32(h + 22), loaded.sp) || !scaleShift(readF32(h + 26), loaded.sc)) {
    LOG_ERR("CPT", "Bad model scales");
    return false;
  }
  loaded.weights = std::move(buffer);
  model = std::move(loaded);
  LOG_INF("CPT", "Concept model loaded");
  return true;
}

// ---- Engram file -------------------------------------------------------------
struct Engram {
  HalFile file;
  uint32_t slots = 0;
  int sg = 0;

  bool open(const char* path) {
    if (!path || !*path || !Storage.exists(path) || !Storage.openFileForRead("CPT", path, file)) return false;
    uint8_t header[20];
    if (file.read(header, sizeof(header)) != static_cast<int>(sizeof(header)) || readU32(header) != ENGR_MAGIC ||
        readU32(header + 4) != 1 || readU32(header + 12) != ENGR_SLOT_BYTES || !scaleShift(readF32(header + 16), sg)) {
      file.close();
      return false;
    }
    slots = readU32(header + 8);
    if (slots == 0 || file.fileSize() < ENGR_HEADER_BYTES + static_cast<size_t>(slots) * ENGR_SLOT_BYTES) {
      file.close();
      slots = 0;
      return false;
    }
    return true;
  }

  bool readSlot(const uint64_t hash, uint8_t slot[ENGR_SLOT_BYTES]) {
    if (slots == 0) return false;
    const size_t offset = ENGR_HEADER_BYTES + static_cast<size_t>(hash % slots) * ENGR_SLOT_BYTES;
    return file.seekSet(offset) && file.read(slot, ENGR_SLOT_BYTES) == static_cast<int>(ENGR_SLOT_BYTES);
  }
};

// ---- Forward pass --------------------------------------------------------------
struct BlockWeights {
  const int8_t* dw;
  const int8_t* pw;
  const int8_t* gate;
  const int8_t* out;  // core blocks only
  int s;
};

// Gated ShortConv (LFM2 double gate, hard-sigmoid in Q7), one step of a causal
// sequence; `y1`/`y2` hold the input-gated values one and two steps back.
template <size_t D>
struct BlockState {
  int8_t y1[D] = {};
  int8_t y2[D] = {};
};

template <size_t D>
void matVec(const int8_t* x, const int8_t* w, int32_t acc[D], const size_t in = D) {
  std::fill(acc, acc + D, 0);
  for (size_t i = 0; i < in; ++i) {
    const int32_t xi = x[i];
    if (xi == 0) continue;
    const int8_t* row = w + i * D;
    for (size_t j = 0; j < D; ++j) acc[j] += xi * row[j];
  }
}

template <size_t D>
void blockStep(const BlockWeights& w, BlockState<D>& state, int8_t x[D]) {
  int32_t acc[D];
  int16_t gs[D];
  int8_t y[D];
  int8_t z[D];
  matVec<D>(x, w.gate, acc);
  for (size_t c = 0; c < D; ++c) {
    gs[c] = static_cast<int16_t>(std::clamp<int32_t>(rq(acc[c], w.s) + 64, 0, 128));
    y[c] = static_cast<int8_t>((x[c] * gs[c] + 64) >> 7);
    const int8_t* tap = w.dw + c * 3;
    z[c] = rq(tap[0] * state.y2[c] + tap[1] * state.y1[c] + tap[2] * y[c], w.s);
  }
  memcpy(state.y2, state.y1, D);
  memcpy(state.y1, y, D);
  matVec<D>(z, w.pw, acc);
  int8_t o2[D];
  for (size_t c = 0; c < D; ++c) o2[c] = static_cast<int8_t>((rq(acc[c], w.s) * gs[c] + 64) >> 7);
  if (!w.out) {
    for (size_t c = 0; c < D; ++c) x[c] = sat8(x[c] + o2[c]);
    return;
  }
  matVec<D>(o2, w.out, acc);
  for (size_t c = 0; c < D; ++c) x[c] = sat8(x[c] + rq(acc[c], w.s));
}

bool continuation(const uint8_t b) { return (b & 0xC0) == 0x80; }
size_t utf8Length(const uint8_t b) { return b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1; }

struct Forward {
  BlockWeights conv1{weightsAt(OFF_C1_DW), weightsAt(OFF_C1_PW), weightsAt(OFF_C1_GATE), nullptr, model.sp};
  BlockWeights conv2{weightsAt(OFF_C2_DW), weightsAt(OFF_C2_PW), weightsAt(OFF_C2_GATE), nullptr, model.sp};
  BlockWeights core{weightsAt(OFF_CORE_DW), weightsAt(OFF_CORE_PW), weightsAt(OFF_CORE_GATE), weightsAt(OFF_CORE_OUT),
                    model.sc};
  BlockState<D_BYTE> conv1State;
  BlockState<D_BYTE> conv2State;
  BlockState<DIM> coreLoop1;
  BlockState<DIM> coreLoop2;
  Engram engram;
  bool engramOpen = false;
  int32_t sums[DIM] = {};

  void chunk(const uint8_t* span, const size_t len, const int32_t pooled[D_BYTE]) {
    const int32_t recip = static_cast<int32_t>((65536 + len / 2) / len);
    int8_t m[D_BYTE];
    for (size_t d = 0; d < D_BYTE; ++d) m[d] = sat8((pooled[d] * recip + 32768) >> 16);
    int32_t acc[DIM];
    matVec<DIM>(m, weightsAt(OFF_CHUNK_PROJ), acc, D_BYTE);
    int8_t x[DIM];
    for (size_t d = 0; d < DIM; ++d) x[d] = rq(acc[d], model.sp);
    uint8_t slot[ENGR_SLOT_BYTES];
    if (engramOpen && engram.readSlot(fnv1a64({reinterpret_cast<const char*>(span), len}), slot)) {
      const auto* delta = reinterpret_cast<const int8_t*>(slot + 8);
      for (size_t d = 0; d < ENGRAM_DIMS; ++d) x[d] = sat8(x[d] + sat8(sh(delta[d], engram.sg - ACT_SHIFT)));
    }
    blockStep<DIM>(core, coreLoop1, x);
    blockStep<DIM>(core, coreLoop2, x);
    for (size_t d = 0; d < DIM; ++d) sums[d] += x[d];
  }

  void run(const uint8_t* bytes, const size_t n) {
    const int8_t* embed = weightsAt(OFF_EMBED);
    int8_t kPrev[D_ROUTER] = {};
    int32_t pooled[D_BYTE] = {};
    size_t start = 0;
    size_t len = 0;
    for (size_t t = 0; t < n; ++t) {
      const uint8_t b = bytes[t];
      int8_t h[D_BYTE];
      for (size_t d = 0; d < D_BYTE; ++d) h[d] = sat8(sh(embed[b * D_BYTE + d], model.se - ACT_SHIFT));
      blockStep<D_BYTE>(conv1, conv1State, h);
      blockStep<D_BYTE>(conv2, conv2State, h);
      int32_t qAcc[D_ROUTER];
      int32_t kAcc[D_ROUTER];
      matVec<D_ROUTER>(h, weightsAt(OFF_ROUTER_Q), qAcc, D_BYTE);
      matVec<D_ROUTER>(h, weightsAt(OFF_ROUTER_K), kAcc, D_BYTE);
      int32_t qk = 0;
      int8_t k[D_ROUTER];
      for (size_t d = 0; d < D_ROUTER; ++d) {
        qk += rq(qAcc[d], model.sr) * kPrev[d];
        k[d] = rq(kAcc[d], model.sr);
      }
      const bool boundary =
          t == 0 || len >= SPAN_MAX || (!continuation(b) && (len + utf8Length(b) > SPAN_MAX || qk <= 0));
      if (boundary) {
        if (t > 0) chunk(bytes + start, len, pooled);
        start = t;
        len = 0;
        std::fill(pooled, pooled + D_BYTE, 0);
      }
      for (size_t d = 0; d < D_BYTE; ++d) pooled[d] += h[d];
      ++len;
      memcpy(kPrev, k, D_ROUTER);
    }
    if (len > 0) chunk(bytes + start, len, pooled);
  }
};

// ---- ICPT files ----------------------------------------------------------------
bool readIcptHeader(HalFile& file, uint32_t& count) {
  uint8_t header[ICPT_HEADER_BYTES];
  if (!file.seekSet(0) || file.read(header, sizeof(header)) != static_cast<int>(sizeof(header))) return false;
  if (readU32(header) != ICPT_MAGIC || readU32(header + 4) != 1 || readU32(header + 12) != DIM) return false;
  const size_t size = file.fileSize();
  const size_t stored = size > ICPT_HEADER_BYTES ? (size - ICPT_HEADER_BYTES) / ICPT_RECORD_BYTES : 0;
  count = std::min<uint32_t>(readU32(header + 8), static_cast<uint32_t>(stored));
  return true;
}

// Calls fn(index, record) for every record; fn returns false to stop.
template <typename Fn>
bool forEachRecord(HalFile& file, Fn&& fn) {
  uint32_t count = 0;
  if (!readIcptHeader(file, count)) return false;
  auto batch = makeUniqueNoThrow<uint8_t[]>(ICPT_BATCH * ICPT_RECORD_BYTES);
  if (!batch) {
    LOG_ERR("CPT", "OOM: index batch");
    return false;
  }
  for (uint32_t first = 0; first < count; first += ICPT_BATCH) {
    const uint32_t n = std::min<uint32_t>(ICPT_BATCH, count - first);
    const int bytes = static_cast<int>(n * ICPT_RECORD_BYTES);
    if (file.read(batch.get(), static_cast<size_t>(bytes)) != bytes) return false;
    for (uint32_t i = 0; i < n; ++i) {
      if (!fn(first + i, batch.get() + i * ICPT_RECORD_BYTES)) return true;
    }
  }
  return true;
}

template <typename Fn>
bool scanIndex(const char* path, Fn&& fn) {
  if (!path || !*path || !Storage.exists(path)) return false;
  HalFile file;
  if (!Storage.openFileForRead("CPT", path, file)) return false;
  const bool ok = forEachRecord(file, fn);
  file.close();
  return ok;
}

std::string recordLabel(const uint8_t* record) {
  const char* label = reinterpret_cast<const char*>(record + ICPT_LABEL_OFFSET);
  return std::string(label, strnlen(label, ICPT_LABEL_BYTES));
}

int32_t recordDot(const uint8_t* record, const int8_t query[DIM]) {
  return dot(reinterpret_cast<const int8_t*>(record + ICPT_VEC_OFFSET), query);
}

// The stem of a path's file name, cut to fit label[48] with its NUL without
// splitting a UTF-8 character.
std::string labelFor(std::string_view path) {
  const size_t slash = path.find_last_of('/');
  if (slash != std::string_view::npos) path.remove_prefix(slash + 1);
  const size_t dot = path.find_last_of('.');
  if (dot != std::string_view::npos && dot > 0) path = path.substr(0, dot);
  size_t len = std::min(path.size(), ICPT_LABEL_BYTES - 1);
  while (len > 0 && len < path.size() && continuation(static_cast<uint8_t>(path[len]))) --len;
  return std::string(path.substr(0, len));
}

void insertTop(std::vector<Match>& top, Match&& match, const size_t maxResults) {
  if (maxResults == 0) return;
  auto it = std::find_if(top.begin(), top.end(), [&](const Match& m) { return match.dot > m.dot; });
  if (top.size() >= maxResults && it == top.end()) return;
  top.insert(it, std::move(match));
  if (top.size() > maxResults) top.pop_back();
}

// ---- Sense parsing ----------------------------------------------------------------
std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

// "12. text" / "3) text": the text after the marker, or npos when not a sense.
size_t numberedSenseStart(std::string_view line) {
  size_t i = 0;
  while (i < line.size() && line[i] == ' ') ++i;
  const size_t digits = i;
  while (i < line.size() && i - digits < 3 && line[i] >= '0' && line[i] <= '9') ++i;
  if (i == digits || i >= line.size() || (line[i] != '.' && line[i] != ')')) return std::string_view::npos;
  ++i;
  if (i < line.size() && line[i] != ' ') return std::string_view::npos;
  return i;
}

bool isLabelLine(std::string_view line) {
  line = trim(line);
  return line.size() >= 2 && line.front() == '(' && line.back() == ')';
}

struct Sense {
  size_t line;
  std::string_view text;    // trimmed sense text
  size_t label;             // line index of its label, or npos
  size_t segment;           // index of the ';' segment within its line (cedict)
};

struct ParsedDefinition {
  std::vector<std::string_view> lines;
  std::vector<Sense> senses;
  bool numbered = false;
  size_t headerEnd = 0;  // lines before this stay above the marked sense
};

std::vector<std::string_view> splitSegments(std::string_view line) {
  std::vector<std::string_view> segments;
  size_t start = 0;
  while (start <= line.size()) {
    const size_t semi = line.find(';', start);
    const size_t end = semi == std::string_view::npos ? line.size() : semi;
    segments.push_back(trim(line.substr(start, end - start)));
    if (semi == std::string_view::npos) break;
    start = semi + 1;
  }
  return segments;
}

ParsedDefinition parseSenses(std::string_view text) {
  ParsedDefinition parsed;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t nl = text.find('\n', start);
    const size_t end = nl == std::string_view::npos ? text.size() : nl;
    parsed.lines.push_back(text.substr(start, end - start));
    if (nl == std::string_view::npos) break;
    start = nl + 1;
  }
  const auto& lines = parsed.lines;
  const size_t npos = std::string_view::npos;
  size_t label = npos;
  for (size_t i = 0; i < lines.size(); ++i) {
    const size_t at = numberedSenseStart(lines[i]);
    if (at != npos) {
      const auto body = trim(lines[i].substr(at));
      if (!body.empty()) parsed.senses.push_back({i, body, label, 0});
    } else if (isLabelLine(lines[i])) {
      label = i;
    }
  }
  if (!parsed.senses.empty()) {
    parsed.numbered = true;
    const Sense& first = parsed.senses.front();
    parsed.headerEnd = first.label != npos ? first.label : first.line;
    return parsed;
  }
  // cedict: blocks of "header\nA; B; C" separated by blank lines, or a single
  // "A; B; C" line. Only "; " lists are senses: other lines may be prose.
  size_t blockStart = 0;
  size_t blocks = 0;
  std::vector<Sense> found;
  while (blockStart < lines.size()) {
    size_t blockEnd = blockStart;
    while (blockEnd < lines.size() && !trim(lines[blockEnd]).empty()) ++blockEnd;
    if (blockEnd > blockStart) {
      ++blocks;
      const bool hasHeader = blockEnd - blockStart > 1;
      for (size_t i = blockStart + (hasHeader ? 1 : 0); i < blockEnd; ++i) {
        if (lines[i].find(';') == std::string_view::npos) continue;
        const auto segments = splitSegments(lines[i]);
        for (size_t s = 0; s < segments.size(); ++s) {
          if (!segments[s].empty()) found.push_back({i, segments[s], hasHeader ? blockStart : npos, s});
        }
      }
    }
    blockStart = blockEnd + 1;
  }
  if (found.empty()) return parsed;
  parsed.headerEnd = found.front().line;
  // One block's header (its pinyin) already sits above the marked sense.
  if (blocks == 1)
    for (auto& sense : found) sense.label = npos;
  parsed.senses = std::move(found);
  return parsed;
}

// Index into parsed.senses of the best match, or npos with fewer than two.
size_t bestSenseIndex(const ParsedDefinition& parsed, const int8_t context[DIM]) {
  if (parsed.senses.size() < 2) return std::string_view::npos;
  size_t best = 0;
  int32_t bestDot = INT32_MIN;
  const size_t scored = std::min(parsed.senses.size(), MAX_SCORED_SENSES);
  for (size_t i = 0; i < scored; ++i) {
    int8_t vec[DIM];
    encodeText(parsed.senses[i].text, vec);
    const int32_t score = dot(context, vec);
    if (score > bestDot) {
      bestDot = score;
      best = i;
    }
  }
  return best;
}

// One code point at text[i]; a malformed byte decodes as itself, length 1.
uint32_t decodeUtf8(const std::string_view text, const size_t i, size_t& length) {
  const auto byte = [&](const size_t k) { return static_cast<uint8_t>(text[k]); };
  const uint8_t lead = byte(i);
  length = 1;
  if (lead < 0x80) return lead;
  const size_t need = lead >= 0xF0 && lead < 0xF8 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 0;
  if (need == 0 || lead >= 0xF8 || i + need > text.size()) return lead;
  uint32_t cp = lead & (0x7F >> need);
  for (size_t k = 1; k < need; ++k) {
    if ((byte(i + k) & 0xC0) != 0x80) return lead;
    cp = (cp << 6) | (byte(i + k) & 0x3F);
  }
  length = need;
  return cp;
}

void appendUtf8(std::string& out, const uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Whitespace and punctuation trimmed from a gloss key: ASCII non-alphanumerics,
// Latin-1 punctuation, general punctuation, CJK/fullwidth punctuation.
bool isGlossPunct(const uint32_t cp) {
  if (cp < 0x80) return !((cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z'));
  if (cp <= 0xBF) return cp != 0xAA && cp != 0xB2 && cp != 0xB3 && cp != 0xB5 && cp != 0xB9 && cp != 0xBA &&
                         !(cp >= 0xBC && cp <= 0xBE);
  if (cp >= 0x2000 && cp <= 0x206F) return true;
  if (cp >= 0x3000 && cp <= 0x303F) return !(cp >= 0x3005 && cp <= 0x3007) && !(cp >= 0x3021 && cp <= 0x3029);
  if (cp == 0x30FB || (cp >= 0xFE30 && cp <= 0xFE4F)) return true;
  return (cp >= 0xFF01 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40) ||
         (cp >= 0xFF5B && cp <= 0xFF65);
}

bool isCjk(const uint32_t cp) {
  return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0xFF66 && cp <= 0xFFDC) || (cp >= 0x20000 && cp <= 0x3FFFF);
}

// Simple lowercase for Latin-1, Latin Extended-A, Greek and Cyrillic capitals.
uint32_t lowerCodepoint(const uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') return cp + 32;
  if (cp < 0xC0) return cp;
  if (cp <= 0xDE) return cp == 0xD7 ? cp : cp + 32;
  if (cp >= 0x100 && cp <= 0x17F) {
    if (cp == 0x130) return 'i';
    if (cp == 0x178) return 0xFF;
    const bool evenUpper = (cp <= 0x137) || (cp >= 0x14A && cp <= 0x177);
    const bool oddUpper = (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E);
    if ((evenUpper && cp % 2 == 0) || (oddUpper && cp % 2 == 1)) return cp + 1;
    return cp;
  }
  if (cp >= 0x391 && cp <= 0x3AB && cp != 0x3A2) return cp + 32;
  if (cp >= 0x410 && cp <= 0x42F) return cp + 32;
  if (cp >= 0x400 && cp <= 0x40F) return cp + 80;
  return cp;
}

}  // namespace

uint64_t fnv1a64(std::string_view bytes) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (const char c : bytes) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

int32_t dot(const int8_t a[DIM], const int8_t b[DIM]) {
  int32_t sum = 0;
  for (size_t d = 0; d < DIM; ++d) sum += static_cast<int32_t>(a[d]) * b[d];
  return sum;
}

void encodeFallback(std::string_view utf8Text, int8_t outVec[DIM]) {
  const size_t n = std::min(utf8Text.size(), MAX_INPUT_BYTES);
  int32_t acc[DIM] = {};
  uint8_t bytes[MAX_INPUT_BYTES];
  for (size_t i = 0; i < n; ++i) {
    const auto b = static_cast<uint8_t>(utf8Text[i]);
    bytes[i] = b >= 'A' && b <= 'Z' ? static_cast<uint8_t>(b + 32) : b;
  }
  const size_t gram = n < 3 ? n : 3;
  const size_t grams = n == 0 ? 0 : n - gram + 1;
  for (size_t i = 0; i < grams; ++i) {
    const uint64_t h = fnv1a64({reinterpret_cast<const char*>(bytes + i), gram});
    const uint64_t h2 = (h * 0x9E3779B97F4A7C15ULL) ^ (h >> 29);
    for (size_t d = 0; d < DIM; ++d) {
      const uint64_t bit = d < 64 ? (h >> d) & 1u : (h2 >> (d - 64)) & 1u;
      acc[d] += bit ? 1 : -1;
    }
  }
  normalize(acc, outVec);
}

bool encodeText(std::string_view utf8Text, int8_t outVec[DIM], const char* engramPath) {
  if (!loadModel()) {
    encodeFallback(utf8Text, outVec);
    return false;
  }
  const size_t n = std::min(utf8Text.size(), MAX_INPUT_BYTES);
  auto forward = makeUniqueNoThrow<Forward>();
  if (!forward) {
    LOG_ERR("CPT", "OOM: forward state");
    encodeFallback(utf8Text, outVec);
    return false;
  }
  forward->engramOpen = forward->engram.open(engramPath);
  forward->run(reinterpret_cast<const uint8_t*>(utf8Text.data()), n);
  if (forward->engramOpen) forward->engram.file.close();
  normalize(forward->sums, outVec);
  return true;
}

std::string bestDictionarySense(const int8_t contextVec[DIM], std::string_view plainDefinition) {
  const ParsedDefinition parsed = parseSenses(plainDefinition);
  const size_t best = bestSenseIndex(parsed, contextVec);
  if (best == std::string_view::npos) return {};
  return std::string(parsed.senses[best].text);
}

std::string rankDictionarySenses(const int8_t contextVec[DIM], std::string_view rawDefinition,
                                 std::string_view marker) {
  const ParsedDefinition parsed = parseSenses(rawDefinition);
  const size_t bestIndex = bestSenseIndex(parsed, contextVec);
  if (bestIndex == std::string_view::npos) return std::string(rawDefinition);
  const Sense& best = parsed.senses[bestIndex];
  const auto& lines = parsed.lines;

  std::string out;
  out.reserve(rawDefinition.size() + marker.size() + 64);
  for (size_t i = 0; i < parsed.headerEnd; ++i) {
    out.append(lines[i]);
    out += '\n';
  }
  out.append(marker);
  if (best.label != std::string_view::npos && best.label >= parsed.headerEnd) {
    out.append(trim(lines[best.label]));
    out += ' ';
  }
  out.append(best.text);
  for (size_t i = parsed.headerEnd; i < lines.size(); ++i) {
    if (i != best.line) {
      out += '\n';
      out.append(lines[i]);
      continue;
    }
    if (parsed.numbered) continue;
    // cedict: drop just this gloss from its "; " list.
    std::string rest;
    const auto segments = splitSegments(lines[i]);
    for (size_t s = 0; s < segments.size(); ++s) {
      if (s == best.segment || segments[s].empty()) continue;
      if (!rest.empty()) rest += "; ";
      rest.append(segments[s]);
    }
    if (!rest.empty()) {
      out += '\n';
      out += rest;
    }
  }
  return out;
}

std::string rankDictionarySenses(std::string_view contextSentence, std::string_view rawDefinition,
                                 std::string_view marker) {
  int8_t context[DIM];
  encodeText(contextSentence, context);
  return rankDictionarySenses(context, rawDefinition, marker);
}

std::string lookupEngramGloss(std::string_view text, std::string* matchedWord, const size_t minCjkChars,
                              const char* engramPath) {
  // The trainer's key: the span without surrounding whitespace/punctuation,
  // lowercased unless it holds CJK.
  size_t start = 0;
  size_t end = text.size();
  while (start < end) {
    size_t length;
    if (!isGlossPunct(decodeUtf8(text, start, length))) break;
    start += length;
  }
  while (end > start) {
    size_t at = end - 1;
    while (at > start && (static_cast<uint8_t>(text[at]) & 0xC0) == 0x80) --at;
    size_t length;
    if (!isGlossPunct(decodeUtf8(text, at, length)) || at + length != end) break;
    end = at;
  }
  const std::string_view word = text.substr(start, end - start);
  if (word.empty()) return {};

  bool cjk = false;
  std::string lower;
  lower.reserve(word.size());
  size_t charEnds[GLOSS_MAX_CJK_CHARS];
  size_t chars = 0;
  for (size_t i = 0; i < word.size();) {
    size_t length;
    const uint32_t cp = decodeUtf8(word, i, length);
    cjk = cjk || isCjk(cp);
    // Book text sets don’t and well‐known with typographic marks; keys use ASCII.
    const uint32_t folded = (cp == 0x2018 || cp == 0x2019) ? '\''
                            : (cp == 0x2010 || cp == 0x2011) ? '-'
                                                             : lowerCodepoint(cp);
    if (folded == cp) {
      lower.append(word.data() + i, length);
    } else {
      appendUtf8(lower, folded);
    }
    i += length;
    if (chars < GLOSS_MAX_CJK_CHARS) charEnds[chars++] = i;
  }

  Engram engram;
  if (!engram.open(engramPath)) return {};
  std::string gloss;
  const auto tryKey = [&](const std::string_view key) {
    const uint64_t hash = fnv1a64(key);
    uint8_t slot[ENGR_SLOT_BYTES];
    if (hash == 0 || !engram.readSlot(hash, slot) || readU64(slot) != hash) return false;
    const char* stored = reinterpret_cast<const char*>(slot + ENGR_GLOSS_OFFSET);
    gloss.assign(stored, strnlen(stored, ENGR_GLOSS_BYTES));
    if (gloss.empty()) return false;
    if (matchedWord) matchedWord->assign(key);
    return true;
  };
  if (!cjk) {
    tryKey(lower);
  } else {
    // A CJK run has no spaces: the longest compound from its start wins.
    for (size_t n = chars; n >= std::max<size_t>(minCjkChars, 1); --n) {
      if (tryKey(word.substr(0, charEnds[n - 1]))) break;
    }
  }
  engram.file.close();
  return gloss;
}

bool appendEpisodicVector(std::string_view bookPath, const uint32_t pageIndex, const int8_t vec[DIM]) {
  if (bookPath.empty()) return false;
  HalFile file = Storage.open(paths.episodic, O_RDWR | O_CREAT);
  if (!file) return false;
  uint8_t header[ICPT_HEADER_BYTES];
  if (file.fileSize() < ICPT_HEADER_BYTES) {
    const uint32_t fields[4] = {ICPT_MAGIC, 1, 0, DIM};
    memcpy(header, fields, sizeof(header));
    if (!file.seekSet(0) || file.write(header, sizeof(header)) != sizeof(header)) {
      file.close();
      return false;
    }
  }
  const uint64_t pathHash = fnv1a64(bookPath);
  uint32_t count = 0;
  uint32_t same = UINT32_MAX;
  uint32_t oldest = 0;
  uint32_t oldestSeq = UINT32_MAX;
  uint32_t newestSeq = 0;
  const bool ok = forEachRecord(file, [&](const uint32_t index, const uint8_t* record) {
    count = index + 1;
    const uint32_t seq = readU32(record + 12);
    if (readU64(record) == pathHash && readU32(record + 8) == pageIndex) same = index;
    if (seq < oldestSeq) {
      oldestSeq = seq;
      oldest = index;
    }
    newestSeq = std::max(newestSeq, seq);
    return true;
  });
  if (!ok) {
    file.close();
    return false;
  }
  const uint32_t target = same != UINT32_MAX ? same : count < MAX_EPISODIC_RECORDS ? count : oldest;
  uint8_t record[ICPT_RECORD_BYTES] = {};
  const uint32_t seq = newestSeq + 1;
  memcpy(record, &pathHash, sizeof(pathHash));
  memcpy(record + 8, &pageIndex, sizeof(pageIndex));
  memcpy(record + 12, &seq, sizeof(seq));
  const std::string label = labelFor(bookPath);
  memcpy(record + ICPT_LABEL_OFFSET, label.data(), label.size());
  memcpy(record + ICPT_VEC_OFFSET, vec, DIM);
  bool written = file.seekSet(ICPT_HEADER_BYTES + static_cast<size_t>(target) * ICPT_RECORD_BYTES) &&
                 file.write(record, sizeof(record)) == sizeof(record);
  if (written && target == count) {
    const uint32_t newCount = count + 1;
    written = file.seekSet(8) && file.write(&newCount, sizeof(newCount)) == sizeof(newCount);
  }
  file.close();
  return written;
}

bool appendEpisodicPage(std::string_view bookPath, const uint32_t pageIndex, std::string_view pageText) {
  int8_t vec[DIM];
  encodeText(pageText, vec);
  return appendEpisodicVector(bookPath, pageIndex, vec);
}

std::vector<Match> queryEpisodicAndShelf(const int8_t queryVec[DIM], const size_t maxResults) {
  std::vector<Match> top;
  for (const bool shelf : {false, true}) {
    scanIndex(shelf ? paths.shelf : paths.episodic, [&](uint32_t, const uint8_t* record) {
      const int32_t score = recordDot(record, queryVec);
      if (top.size() < maxResults || score > top.back().dot) {
        insertTop(top, Match{readU64(record), readU32(record + 8), score, shelf, recordLabel(record)}, maxResults);
      }
      return true;
    });
  }
  return top;
}

std::vector<Match> queryEpisodicAndShelf(std::string_view queryText, const size_t maxResults) {
  int8_t query[DIM];
  encodeText(queryText, query);
  return queryEpisodicAndShelf(query, maxResults);
}

bool latestEpisodicVector(int8_t outVec[DIM]) {
  bool found = false;
  uint32_t newest = 0;
  scanIndex(paths.episodic, [&](uint32_t, const uint8_t* record) {
    const uint32_t seq = readU32(record + 12);
    if (!found || seq >= newest) {
      newest = seq;
      memcpy(outVec, record + ICPT_VEC_OFFSET, DIM);
      found = true;
    }
    return true;
  });
  return found;
}

size_t scoreShelf(const int8_t queryVec[DIM], const uint64_t* pathHashes, const size_t count, int32_t* outDots) {
  std::fill(outDots, outDots + count, INT32_MIN);
  size_t matched = 0;
  scanIndex(paths.shelf, [&](uint32_t, const uint8_t* record) {
    const uint64_t hash = readU64(record);
    for (size_t i = 0; i < count; ++i) {
      if (pathHashes[i] != hash || outDots[i] != INT32_MIN) continue;
      outDots[i] = recordDot(record, queryVec);
      ++matched;
    }
    return matched < count;
  });
  return matched;
}

std::string describeMatch(const Match& match) {
  if (match.shelf) return match.label;
  char position[32];
  const uint32_t chapter = match.page >> 16;
  if (chapter > 0) {
    snprintf(position, sizeof(position), " ch.%u p.%u", static_cast<unsigned>(chapter),
             static_cast<unsigned>((match.page & 0xFFFF) + 1));
  } else {
    snprintf(position, sizeof(position), " p.%u", static_cast<unsigned>(match.page + 1));
  }
  return match.label + position;
}

void setPaths(const Paths& newPaths) {
  if (!paths.model || !newPaths.model || strcmp(paths.model, newPaths.model) != 0) unloadModel();
  paths = newPaths;
}

void unloadModel() { model = Model{}; }

}  // namespace ConceptEngine
