#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "util/ConceptEngine.h"

namespace fs = std::filesystem;
namespace ce = ConceptEngine;

namespace {

constexpr size_t DB = 64;
constexpr size_t DC = 128;
constexpr size_t DR = 32;

// ---- File builders ------------------------------------------------------------
template <typename T>
void put(std::vector<uint8_t>& out, const T value) {
  const auto* p = reinterpret_cast<const uint8_t*>(&value);
  out.insert(out.end(), p, p + sizeof(T));
}

void writeFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

struct Shifts {
  int se = 5, sr = 7, sp = 7, sc = 8;
};

// PLHN v1 with deterministic pseudo-random weights in [-range, range].
std::vector<uint8_t> makeModel(const Shifts& s, const uint32_t seed = 12345, const int range = 48) {
  std::vector<uint8_t> out;
  put<uint32_t>(out, 0x504C484E);
  put<uint16_t>(out, 1);
  put<uint16_t>(out, DB);
  put<uint16_t>(out, DC);
  out.push_back(2);
  out.push_back(2);
  out.push_back(DR);
  out.push_back(16);
  put<float>(out, std::ldexp(1.0f, -s.se));
  put<float>(out, std::ldexp(1.0f, -s.sr));
  put<float>(out, std::ldexp(1.0f, -s.sp));
  put<float>(out, std::ldexp(1.0f, -s.sc));
  put<uint16_t>(out, 0);
  EXPECT_EQ(out.size(), 32u);
  uint32_t state = seed;
  while (out.size() < 95008) {
    state = state * 1664525u + 1013904223u;
    const int v = static_cast<int>((state >> 16) % static_cast<uint32_t>(2 * range + 1)) - range;
    out.push_back(static_cast<uint8_t>(static_cast<int8_t>(v)));
  }
  return out;
}

struct EngramSlot {
  uint64_t hash;
  std::array<int8_t, 24> delta;
  std::string gloss;
};

std::vector<uint8_t> makeEngram(const uint32_t slots, const std::vector<EngramSlot>& entries, const int sg = 5) {
  std::vector<uint8_t> out;
  put<uint32_t>(out, 0x454E4752);
  put<uint32_t>(out, 1);
  put<uint32_t>(out, slots);
  put<uint32_t>(out, 64);
  put<float>(out, std::ldexp(1.0f, -sg));
  out.resize(512 + static_cast<size_t>(slots) * 64, 0);
  for (const auto& e : entries) {
    uint8_t* slot = out.data() + 512 + (e.hash % slots) * 64;
    memcpy(slot, &e.hash, 8);
    memcpy(slot + 8, e.delta.data(), 24);
    memcpy(slot + 32, e.gloss.data(), std::min<size_t>(e.gloss.size(), 31));
  }
  return out;
}

struct IcptRecord {
  uint64_t pathHash;
  uint32_t page;
  uint32_t flags;
  std::string label;
  std::array<int8_t, 128> vec;
};

std::vector<uint8_t> makeIcpt(const std::vector<IcptRecord>& records) {
  std::vector<uint8_t> out;
  put<uint32_t>(out, 0x49435054);
  put<uint32_t>(out, 1);
  put<uint32_t>(out, static_cast<uint32_t>(records.size()));
  put<uint32_t>(out, 128);
  for (const auto& r : records) {
    const size_t at = out.size();
    out.resize(at + 192, 0);
    memcpy(out.data() + at, &r.pathHash, 8);
    memcpy(out.data() + at + 8, &r.page, 4);
    memcpy(out.data() + at + 12, &r.flags, 4);
    memcpy(out.data() + at + 16, r.label.data(), std::min<size_t>(r.label.size(), 47));
    memcpy(out.data() + at + 64, r.vec.data(), 128);
  }
  return out;
}

// ---- Literal (non-streaming) reference of plhn-int-spec ------------------------------
int32_t sh(const int64_t a, const int s) {
  if (s > 0) return static_cast<int32_t>((a + (int64_t{1} << (s - 1))) >> s);
  if (s < 0) return static_cast<int32_t>(a * (int64_t{1} << -s));
  return static_cast<int32_t>(a);
}
int8_t sat8(const int64_t a) { return static_cast<int8_t>(a < -127 ? -127 : a > 127 ? 127 : a); }
int8_t rq(const int64_t a, const int s) { return sat8(sh(a, s)); }

using Seq = std::vector<std::vector<int8_t>>;

Seq refBlock(const Seq& x, const size_t d, const int8_t* dw, const int8_t* pw, const int8_t* gate, const int8_t* out,
             const int s) {
  const size_t n = x.size();
  Seq gs(n, std::vector<int8_t>(d)), y(n, std::vector<int8_t>(d)), result(n, std::vector<int8_t>(d));
  std::vector<std::vector<int>> gsq(n, std::vector<int>(d));
  for (size_t t = 0; t < n; ++t) {
    for (size_t j = 0; j < d; ++j) {
      int64_t acc = 0;
      for (size_t i = 0; i < d; ++i) acc += x[t][i] * gate[i * d + j];
      const int g = rq(acc, s);
      gsq[t][j] = std::clamp(g + 64, 0, 128);
      y[t][j] = static_cast<int8_t>((x[t][j] * gsq[t][j] + 64) >> 7);
    }
  }
  for (size_t t = 0; t < n; ++t) {
    std::vector<int8_t> z(d), o2(d);
    for (size_t c = 0; c < d; ++c) {
      const int y2 = t >= 2 ? y[t - 2][c] : 0;
      const int y1 = t >= 1 ? y[t - 1][c] : 0;
      z[c] = rq(dw[c * 3] * y2 + dw[c * 3 + 1] * y1 + dw[c * 3 + 2] * y[t][c], s);
    }
    for (size_t j = 0; j < d; ++j) {
      int64_t acc = 0;
      for (size_t c = 0; c < d; ++c) acc += z[c] * pw[c * d + j];
      o2[j] = static_cast<int8_t>((rq(acc, s) * gsq[t][j] + 64) >> 7);
    }
    for (size_t j = 0; j < d; ++j) {
      if (!out) {
        result[t][j] = sat8(x[t][j] + o2[j]);
      } else {
        int64_t acc = 0;
        for (size_t i = 0; i < d; ++i) acc += o2[i] * out[i * d + j];
        result[t][j] = sat8(x[t][j] + rq(acc, s));
      }
    }
  }
  return result;
}

uint64_t fnv(const uint8_t* p, const size_t n) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ULL;
  return h;
}

using Spans = std::vector<std::pair<size_t, size_t>>;  // (start, length) per H-Net chunk

std::array<int8_t, 128> refForward(const std::vector<uint8_t>& model, const Shifts& s, const std::string& text,
                                   const std::vector<uint8_t>* engram = nullptr, const int sg = 5,
                                   Spans* outSpans = nullptr) {
  const auto* w = reinterpret_cast<const int8_t*>(model.data());
  const size_t embed = 32, c1dw = embed + 256 * DB, c1pw = c1dw + DB * 3, c1g = c1pw + DB * DB, c2dw = c1g + DB * DB,
               c2pw = c2dw + DB * 3, c2g = c2pw + DB * DB, rqo = c2g + DB * DB, rko = rqo + DB * DR,
               cp = rko + DB * DR, cdw = cp + DB * DC, cpw = cdw + DC * 3, cg = cpw + DC * DC, co = cg + DC * DC;
  const size_t n = std::min<size_t>(text.size(), 256);
  const auto* b = reinterpret_cast<const uint8_t*>(text.data());
  std::array<int8_t, 128> out{};
  if (n == 0) return out;
  Seq h(n, std::vector<int8_t>(DB));
  for (size_t t = 0; t < n; ++t)
    for (size_t d = 0; d < DB; ++d) h[t][d] = sat8(sh(w[embed + b[t] * DB + d], s.se - 5));
  h = refBlock(h, DB, w + c1dw, w + c1pw, w + c1g, nullptr, s.sp);
  h = refBlock(h, DB, w + c2dw, w + c2pw, w + c2g, nullptr, s.sp);
  Seq q(n, std::vector<int8_t>(DR)), k(n, std::vector<int8_t>(DR));
  for (size_t t = 0; t < n; ++t)
    for (size_t j = 0; j < DR; ++j) {
      int64_t aq = 0, ak = 0;
      for (size_t i = 0; i < DB; ++i) {
        aq += h[t][i] * w[rqo + i * DR + j];
        ak += h[t][i] * w[rko + i * DR + j];
      }
      q[t][j] = rq(aq, s.sr);
      k[t][j] = rq(ak, s.sr);
    }
  Spans spans;
  size_t len = 0;
  for (size_t t = 0; t < n; ++t) {
    const bool cont = (b[t] & 0xC0) == 0x80;
    const size_t u8 = b[t] < 0x80 ? 1 : b[t] >= 0xF0 ? 4 : b[t] >= 0xE0 ? 3 : b[t] >= 0xC0 ? 2 : 1;
    int64_t dotqk = 0;
    if (t > 0)
      for (size_t j = 0; j < DR; ++j) dotqk += q[t][j] * k[t - 1][j];
    if (t == 0 || len >= 12 || (!cont && (len + u8 > 12 || dotqk <= 0))) {
      spans.push_back({t, 0});
      len = 0;
    }
    ++len;
    spans.back().second = len;
  }
  if (outSpans) *outSpans = spans;
  Seq x(spans.size(), std::vector<int8_t>(DC));
  for (size_t c = 0; c < spans.size(); ++c) {
    const auto [start, count] = spans[c];
    const int64_t recip = (65536 + count / 2) / count;
    std::vector<int8_t> m(DB);
    for (size_t d = 0; d < DB; ++d) {
      int64_t sum = 0;
      for (size_t t = start; t < start + count; ++t) sum += h[t][d];
      m[d] = sat8((sum * recip + 32768) >> 16);
    }
    for (size_t j = 0; j < DC; ++j) {
      int64_t acc = 0;
      for (size_t i = 0; i < DB; ++i) acc += m[i] * w[cp + i * DC + j];
      x[c][j] = rq(acc, s.sp);
    }
    if (engram) {
      uint32_t slots;
      memcpy(&slots, engram->data() + 8, 4);
      const uint64_t key = fnv(b + start, count);
      const auto* delta = reinterpret_cast<const int8_t*>(engram->data() + 512 + (key % slots) * 64 + 8);
      for (size_t d = 0; d < 24; ++d) x[c][d] = sat8(x[c][d] + sat8(sh(delta[d], sg - 5)));
      // Bigram: the previous chunk joined with this one, exact hash only, half delta.
      if (c >= 1 && spans[c - 1].second + count <= 18) {
        const uint64_t key2 = fnv(b + spans[c - 1].first, spans[c - 1].second + count);
        const uint8_t* slot2 = engram->data() + 512 + (key2 % slots) * 64;
        uint64_t stored;
        memcpy(&stored, slot2, 8);
        const auto* delta2 = reinterpret_cast<const int8_t*>(slot2 + 8);
        if (key2 != 0 && stored == key2)
          for (size_t d = 0; d < 24; ++d) x[c][d] = sat8(x[c][d] + sat8(sh(delta2[d] >> 1, sg - 5)));
      }
    }
  }
  for (int loop = 0; loop < 2; ++loop) x = refBlock(x, DC, w + cdw, w + cpw, w + cg, w + co, s.sc);
  std::array<int64_t, 128> sum{};
  for (const auto& chunk : x)
    for (size_t d = 0; d < DC; ++d) sum[d] += chunk[d];
  int64_t n2 = 0;
  for (const auto v : sum) n2 += v * v;
  auto norm = static_cast<int64_t>(std::sqrt(static_cast<long double>(n2)));
  while (norm * norm > n2) --norm;
  while ((norm + 1) * (norm + 1) <= n2) ++norm;
  if (norm == 0) return out;
  for (size_t d = 0; d < DC; ++d) {
    const int64_t a = 127 * sum[d];
    const int64_t r = a >= 0 ? (a + norm / 2) / norm : -((-a + norm / 2) / norm);
    out[d] = static_cast<int8_t>(std::clamp<int64_t>(r, -127, 127));
  }
  return out;
}

std::array<int8_t, 128> encode(const std::string& text, const char* engram = "") {
  std::array<int8_t, 128> v{};
  ce::encodeText(text, v.data(), engram);
  return v;
}

class ConceptEngineTest : public ::testing::Test {
 protected:
  fs::path dir;
  std::string modelPath, engramPath, episodicPath, shelfPath, cspwPath, storyPath;
  // Golden-file locations; members because setPaths keeps the pointers until TearDown.
  std::string goldenModel, goldenEngram, goldenCspw;

  ce::Paths scratchPaths() const {
    ce::Paths paths;
    paths.model = modelPath.c_str();
    paths.episodic = episodicPath.c_str();
    paths.shelf = shelfPath.c_str();
    paths.cspw = cspwPath.c_str();
    paths.storyGraph = storyPath.c_str();
    return paths;
  }

  void SetUp() override {
    dir = fs::temp_directory_path() /
          ("concept_engine_" + std::to_string(getpid()) + "_" +
           ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(dir);
    fs::create_directories(dir);
    modelPath = (dir / "platonic-hnet.bin").string();
    engramPath = (dir / "engram.bin").string();
    episodicPath = (dir / "episodic.idx").string();
    shelfPath = (dir / "inbox-concepts.idx").string();
    cspwPath = (dir / "cspw.bin").string();
    storyPath = (dir / "story-graph.idx").string();
    ce::setPaths(scratchPaths());
    ce::unloadModel();
  }

  void TearDown() override {
    ce::unloadModel();
    ce::clearCurrentContext();
    ce::setPaths(ce::Paths{});
    fs::remove_all(dir);
  }
};

const char* const SENTENCES[] = {
    "The quick brown fox jumps over the lazy dog.",
    "我们在河边散步，看见很多鱼。",
    "a",
    "Ünïcödé — mixed scripts: Привет, 世界! 🙂 and long-running hyphenated-compound-words",
    "The river bank was muddy after the spring flood washed the reeds away from the shore and into the town.",
};

}  // namespace

// ---- PLHN forward ------------------------------------------------------------------

TEST_F(ConceptEngineTest, ForwardMatchesLiteralReferenceBitExact) {
  const Shifts s;
  const auto model = makeModel(s);
  writeFile(modelPath, model);
  for (const char* text : SENTENCES) {
    std::array<int8_t, 128> got{};
    ASSERT_TRUE(ce::encodeText(text, got.data(), "")) << text;
    EXPECT_EQ(got, refForward(model, s, text)) << text;
  }
  // Inputs past 256 bytes are cut at 256.
  const std::string longText(600, 'x');
  EXPECT_EQ(encode(longText), refForward(model, s, longText.substr(0, 256)));
}

TEST_F(ConceptEngineTest, ForwardOutputIsUnitNormAtScale127) {
  writeFile(modelPath, makeModel(Shifts{}));
  const auto v = encode(SENTENCES[0]);
  int64_t n2 = 0;
  for (const int8_t x : v) n2 += x * x;
  EXPECT_NEAR(std::sqrt(static_cast<double>(n2)), 127.0, 6.0);
  EXPECT_EQ(encode(""), (std::array<int8_t, 128>{}));
}

TEST_F(ConceptEngineTest, ForwardAppliesEngramDeltasPerSpan) {
  const Shifts s;
  const auto model = makeModel(s);
  writeFile(modelPath, model);
  // Every slot carries a delta, so every span is nudged.
  std::vector<uint8_t> engram = makeEngram(64, {});
  for (uint32_t slot = 0; slot < 64; ++slot)
    for (size_t d = 0; d < 24; ++d) engram[512 + slot * 64 + 8 + d] = static_cast<uint8_t>(int8_t((slot * 7 + d) % 61 - 30));
  writeFile(engramPath, engram);
  const std::string text = SENTENCES[4];
  const auto with = encode(text, engramPath.c_str());
  EXPECT_EQ(with, refForward(model, s, text, &engram));
  EXPECT_NE(with, encode(text, ""));
}

TEST_F(ConceptEngineTest, RejectsMalformedModelAndFallsBack) {
  auto model = makeModel(Shifts{});
  model[6] = 32;  // d_byte
  writeFile(modelPath, model);
  std::array<int8_t, 128> got{};
  EXPECT_FALSE(ce::encodeText("hello world", got.data(), ""));
  std::array<int8_t, 128> fallback{};
  ce::encodeFallback("hello world", fallback.data());
  EXPECT_EQ(got, fallback);
}

TEST_F(ConceptEngineTest, MissingModelUsesTrigramSimHash) {
  std::array<int8_t, 128> a{}, b{}, c{};
  EXPECT_FALSE(ce::encodeText("Bank of the river", a.data()));
  ce::encodeFallback("bank of the river", b.data());
  EXPECT_EQ(a, b);  // ASCII case folds
  ce::encodeFallback("quantum chromodynamics lecture", c.data());
  EXPECT_GT(ce::dot(a.data(), b.data()), ce::dot(a.data(), c.data()));
  // A model copied later is picked up without a restart.
  writeFile(modelPath, makeModel(Shifts{}));
  EXPECT_TRUE(ce::encodeText("Bank of the river", a.data()));
}

// Golden vectors from the trainer's Python reference:
// CONCEPT_ENGINE_GOLDEN_DIR=<dir with platonic-hnet.bin, engram.bin, golden.jsonl>.
TEST_F(ConceptEngineTest, MatchesTrainerGoldenVectors) {
  const char* goldenDir = std::getenv("CONCEPT_ENGINE_GOLDEN_DIR");
  if (!goldenDir || !*goldenDir) GTEST_SKIP() << "CONCEPT_ENGINE_GOLDEN_DIR not set";
  const fs::path golden(goldenDir);
  goldenModel = (golden / "platonic-hnet.bin").string();
  goldenEngram = (golden / "engram.bin").string();
  ce::Paths paths = scratchPaths();
  paths.model = goldenModel.c_str();
  ce::setPaths(paths);
  std::ifstream lines(golden / "golden.jsonl");
  ASSERT_TRUE(lines.good());
  std::string line;
  size_t checked = 0;
  while (std::getline(lines, line)) {
    // {"text": "<json string>", "vec": [ints]}
    const size_t key = line.find("\"text\"");
    ASSERT_NE(key, std::string::npos);
    size_t i = line.find('"', line.find(':', key)) + 1;
    std::string text;
    for (; line[i] != '"'; ++i) {
      if (line[i] != '\\') {
        text += line[i];
        continue;
      }
      const char e = line[++i];
      if (e == 'n') text += '\n';
      else if (e == 't') text += '\t';
      else if (e == 'r') text += '\r';
      else if (e == 'u') {
        uint32_t cp = std::stoul(line.substr(i + 1, 4), nullptr, 16);
        i += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && line[i + 1] == '\\' && line[i + 2] == 'u') {
          const uint32_t low = std::stoul(line.substr(i + 3, 4), nullptr, 16);
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          i += 6;
        }
        if (cp < 0x80) text += static_cast<char>(cp);
        else if (cp < 0x800) text += {static_cast<char>(0xC0 | cp >> 6), static_cast<char>(0x80 | (cp & 0x3F))};
        else if (cp < 0x10000)
          text += {static_cast<char>(0xE0 | cp >> 12), static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
                   static_cast<char>(0x80 | (cp & 0x3F))};
        else
          text += {static_cast<char>(0xF0 | cp >> 18), static_cast<char>(0x80 | ((cp >> 12) & 0x3F)),
                   static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), static_cast<char>(0x80 | (cp & 0x3F))};
      } else {
        text += e;
      }
    }
    std::array<int8_t, 128> want{};
    std::stringstream vec(line.substr(line.find('[', line.find("\"vec\"")) + 1));
    for (auto& v : want) {
      int value;
      char sep;
      vec >> value >> sep;
      v = static_cast<int8_t>(value);
    }
    std::array<int8_t, 128> got{};
    ASSERT_TRUE(ce::encodeText(text, got.data(), goldenEngram.c_str()));
    EXPECT_EQ(got, want) << text;
    ++checked;
  }
  EXPECT_GT(checked, 0u);
}

// ---- ENGR gloss ------------------------------------------------------------------

TEST_F(ConceptEngineTest, EngramGlossNeedsExactSpanHash) {
  constexpr uint32_t slots = 64;
  const uint64_t rizz = ce::fnv1a64("rizz");
  writeFile(engramPath, makeEngram(slots, {{rizz, {}, "charisma, esp. in flirting"}}));
  EXPECT_EQ(ce::lookupEngramGloss("rizz", engramPath.c_str()), "charisma, esp. in flirting");
  EXPECT_EQ(ce::lookupEngramGloss("Rizz", engramPath.c_str()), "charisma, esp. in flirting");
  // A word landing in the same slot with a different hash is not a match.
  std::string collider;
  for (int i = 0; collider.empty(); ++i) {
    const std::string candidate = "w" + std::to_string(i);
    if (ce::fnv1a64(candidate) % slots == rizz % slots) collider = candidate;
  }
  EXPECT_EQ(ce::lookupEngramGloss(collider, engramPath.c_str()), "");
  EXPECT_EQ(ce::lookupEngramGloss("rizz", (dir / "absent.bin").c_str()), "");
  EXPECT_EQ(ce::lookupEngramGloss("", engramPath.c_str()), "");
}

TEST_F(ConceptEngineTest, EngramGlossRejectsBadHeader) {
  auto engram = makeEngram(64, {{ce::fnv1a64("yeet"), {}, "to throw"}});
  engram[12] = 32;  // slot_bytes
  writeFile(engramPath, engram);
  EXPECT_EQ(ce::lookupEngramGloss("yeet", engramPath.c_str()), "");
}

// ---- Sense ranking ------------------------------------------------------------------

TEST_F(ConceptEngineTest, RanksContextualWordNetSenseFirst) {
  const std::string definition =
      "(noun)\n"
      "1. a financial institution that accepts deposits and lends money\n"
      "2. sloping land beside a river or other body of water\n"
      "(verb)\n"
      "1. tilt an aircraft while turning";
  const std::string ranked =
      ce::rankDictionarySenses("we sat on the grassy bank of the river and watched the water", definition);
  EXPECT_EQ(ranked,
            "\xE2\x98\x85 (noun) sloping land beside a river or other body of water\n"
            "(noun)\n"
            "1. a financial institution that accepts deposits and lends money\n"
            "(verb)\n"
            "1. tilt an aircraft while turning");
  const std::string money = ce::rankDictionarySenses("she went to the bank to deposit money", definition);
  EXPECT_EQ(money.rfind("\xE2\x98\x85 (noun) a financial institution", 0), 0u) << money;
  EXPECT_NE(money.find("2. sloping land"), std::string::npos);
}

TEST_F(ConceptEngineTest, RanksCedictGlossesUnderPinyin) {
  // The trigram fallback needs a few shared trigrams, so the glosses are phrases.
  const std::string definition = "hé\nriver flowing through a valley; harmony between people; peace and calm";
  int8_t context[128];
  ce::encodeText("the boat drifted down the river flowing through the valley", context);
  EXPECT_EQ(ce::rankDictionarySenses(context, definition, "* "),
            "hé\n* river flowing through a valley\nharmony between people; peace and calm");
  EXPECT_EQ(ce::bestDictionarySense(context, definition), "river flowing through a valley");
}

TEST_F(ConceptEngineTest, LeavesSingleSenseDefinitionsAlone) {
  EXPECT_EQ(ce::rankDictionarySenses("anything", "a single gloss"), "a single gloss");
  EXPECT_EQ(ce::rankDictionarySenses("anything", "(noun)\n1. only sense"), "(noun)\n1. only sense");
  EXPECT_EQ(ce::rankDictionarySenses("anything", "line one\nline two"), "line one\nline two");
  EXPECT_EQ(ce::rankDictionarySenses("anything", ""), "");
}

// ---- ICPT episodic + shelf ------------------------------------------------------------

TEST_F(ConceptEngineTest, EpisodicRecallFindsPagePreviouslyRead) {
  ASSERT_TRUE(ce::appendEpisodicPage("/books/Moby Dick.epub", ce::epubPosition(3, 11),
                                     "the whale breached beside the harpoon boat at dawn"));
  ASSERT_TRUE(ce::appendEpisodicPage("/books/Cookbook.txt", 41, "whisk the eggs with sugar and butter"));
  ASSERT_TRUE(ce::appendEpisodicPage("/books/Physics.epub", 7, "electrons orbit the nucleus in shells"));
  EXPECT_EQ(fs::file_size(episodicPath), 16u + 3 * 192);

  const auto matches = ce::queryEpisodicAndShelf("whisk eggs and sugar", 2);
  ASSERT_EQ(matches.size(), 2u);
  EXPECT_EQ(matches[0].label, "Cookbook");
  EXPECT_EQ(matches[0].pathHash, ce::fnv1a64("/books/Cookbook.txt"));
  EXPECT_FALSE(matches[0].shelf);
  EXPECT_GE(matches[0].dot, matches[1].dot);
  EXPECT_EQ(ce::describeMatch(matches[0]), "Cookbook p.42");

  const auto whale = ce::queryEpisodicAndShelf("the whale breached beside the harpoon boat at dawn", 1);
  ASSERT_EQ(whale.size(), 1u);
  EXPECT_EQ(ce::describeMatch(whale[0]), "Moby Dick ch.4 p.12");
  EXPECT_GE(whale[0].dot, ce::RELATED_MIN_DOT);

  int8_t latest[128];
  int8_t physics[128];
  ASSERT_TRUE(ce::latestEpisodicVector(latest));
  ce::encodeText("electrons orbit the nucleus in shells", physics);
  EXPECT_EQ(memcmp(latest, physics, 128), 0);
}

TEST_F(ConceptEngineTest, EpisodicSamePageIsReplacedNotDuplicated) {
  ASSERT_TRUE(ce::appendEpisodicPage("/books/a.txt", 5, "first visit text"));
  ASSERT_TRUE(ce::appendEpisodicPage("/books/a.txt", 6, "next page"));
  ASSERT_TRUE(ce::appendEpisodicPage("/books/a.txt", 5, "second visit text"));
  EXPECT_EQ(fs::file_size(episodicPath), 16u + 2 * 192);
  int8_t latest[128];
  int8_t second[128];
  ASSERT_TRUE(ce::latestEpisodicVector(latest));
  ce::encodeText("second visit text", second);
  EXPECT_EQ(memcmp(latest, second, 128), 0);
}

TEST_F(ConceptEngineTest, EpisodicRefusesForeignFile) {
  writeFile(episodicPath, std::vector<uint8_t>(64, 0xAB));
  EXPECT_FALSE(ce::appendEpisodicPage("/books/a.txt", 1, "text"));
  EXPECT_EQ(fs::file_size(episodicPath), 64u);
  EXPECT_TRUE(ce::queryEpisodicAndShelf("text").empty());
}

TEST_F(ConceptEngineTest, ShelfRecordsJoinQueriesAndScores) {
  IcptRecord sailing{ce::fnv1a64("/books/Inbox/sailing.epub"), 0, 1700000000, "Learning to Sail", {}};
  IcptRecord baking{ce::fnv1a64("/books/Inbox/baking.epub"), 0, 1700000001, "Bread Basics", {}};
  ce::encodeText("wind fills the sails as the boat heels", sailing.vec.data());
  ce::encodeText("knead the dough and let the bread rise", baking.vec.data());
  writeFile(shelfPath, makeIcpt({sailing, baking}));
  ASSERT_TRUE(ce::appendEpisodicPage("/books/Moby Dick.epub", 3, "harpoons and whales"));

  const auto matches = ce::queryEpisodicAndShelf("the wind filled the sails of the boat", 3);
  ASSERT_EQ(matches.size(), 3u);
  EXPECT_TRUE(matches[0].shelf);
  EXPECT_EQ(ce::describeMatch(matches[0]), "Learning to Sail");

  int8_t query[128];
  ce::encodeText("let the dough rise before baking bread", query);
  const uint64_t hashes[3] = {sailing.pathHash, ce::fnv1a64("/books/Inbox/unknown.epub"), baking.pathHash};
  int32_t dots[3];
  EXPECT_EQ(ce::scoreShelf(query, hashes, 3, dots), 2u);
  EXPECT_EQ(dots[1], INT32_MIN);
  EXPECT_GT(dots[2], dots[0]);
  EXPECT_EQ(dots[2], ce::dot(query, baking.vec.data()));
}

TEST_F(ConceptEngineTest, QueryCanLeaveOutTheShelf) {
  IcptRecord sailing{ce::fnv1a64("/books/Inbox/sailing.epub"), 0, 1700000000, "Learning to Sail", {}};
  ce::encodeText("wind fills the sails as the boat heels", sailing.vec.data());
  writeFile(shelfPath, makeIcpt({sailing}));
  ASSERT_TRUE(ce::appendEpisodicPage("/books/Moby Dick.epub", 3, "harpoons and whales"));

  int8_t query[128];
  ce::encodeText("wind fills the sails as the boat heels", query);
  const auto all = ce::queryEpisodicAndShelf(query, 3);
  ASSERT_EQ(all.size(), 2u);
  EXPECT_TRUE(all[0].shelf);

  const auto episodicOnly = ce::queryEpisodicAndShelf(query, 3, /*includeShelf=*/false);
  ASSERT_EQ(episodicOnly.size(), 1u);
  EXPECT_FALSE(episodicOnly[0].shelf);
  EXPECT_EQ(episodicOnly[0].label, "Moby Dick");
}

TEST_F(ConceptEngineTest, CurrentContextWinsOverNewestEpisodicPage) {
  int8_t out[128];
  EXPECT_FALSE(ce::currentContext(out));
  EXPECT_FALSE(ce::readingContext(out));

  ASSERT_TRUE(ce::appendEpisodicPage("/books/a.txt", 1, "electrons orbit the nucleus"));
  int8_t episodic[128];
  ce::encodeText("electrons orbit the nucleus", episodic);
  ASSERT_TRUE(ce::readingContext(out));
  EXPECT_EQ(memcmp(out, episodic, 128), 0);

  int8_t current[128];
  ce::encodeText("knead the dough and let the bread rise", current);
  ce::setCurrentContext(current);
  ASSERT_TRUE(ce::currentContext(out));
  EXPECT_EQ(memcmp(out, current, 128), 0);
  ASSERT_TRUE(ce::readingContext(out));
  EXPECT_EQ(memcmp(out, current, 128), 0);

  ce::clearCurrentContext();
  EXPECT_FALSE(ce::currentContext(out));
}

TEST_F(ConceptEngineTest, PageDwellSetsContextThenRecordsEpisode) {
  const std::string_view slice = "the whale breached beside the harpoon boat";
  int8_t out[128];
  int8_t expected[128];
  ce::encodeText(slice, expected);

  EXPECT_FALSE(ce::recordPageDwell("/books/m.epub", 7, slice, ce::CONTEXT_DWELL_MS - 1));
  EXPECT_FALSE(ce::currentContext(out));
  EXPECT_FALSE(ce::recordPageDwell("/books/m.epub", 7, "", ce::EPISODE_DWELL_MS));
  EXPECT_FALSE(ce::currentContext(out));

  EXPECT_TRUE(ce::recordPageDwell("/books/m.epub", 7, slice, ce::CONTEXT_DWELL_MS));
  ASSERT_TRUE(ce::currentContext(out));
  EXPECT_EQ(memcmp(out, expected, 128), 0);
  EXPECT_FALSE(fs::exists(episodicPath));

  EXPECT_TRUE(ce::recordPageDwell("/books/m.epub", 7, slice, ce::EPISODE_DWELL_MS));
  ASSERT_TRUE(fs::exists(episodicPath));
  EXPECT_EQ(fs::file_size(episodicPath), 16u + 192);
  ASSERT_TRUE(ce::latestEpisodicVector(out));
  EXPECT_EQ(memcmp(out, expected, 128), 0);
}

TEST(ConceptEngineDwell, QueuesOnlyPagesLookedAtLongEnough) {
  ce::PageDwellQueue queue;
  ce::PageDwellQueue::Page page;
  EXPECT_TRUE(queue.frameShown(0, 1, 1000).append("first page"));
  EXPECT_FALSE(queue.take(page, 999999, 0));  // the first frame has no predecessor

  queue.frameShown(ce::CONTEXT_DWELL_MS - 1, 2, 2000).append("second page");
  EXPECT_FALSE(queue.take(page, 999999, 0));  // page 1 was only glanced at

  queue.frameShown(ce::CONTEXT_DWELL_MS + 5, 3, 30000);
  EXPECT_FALSE(queue.take(page, 30000 + ce::DWELL_SETTLE_MS - 1, ce::DWELL_SETTLE_MS));
  ASSERT_TRUE(queue.take(page, 30000 + ce::DWELL_SETTLE_MS, ce::DWELL_SETTLE_MS));
  EXPECT_EQ(page.index, 2u);
  EXPECT_EQ(page.dwellMs, ce::CONTEXT_DWELL_MS + 5);
  EXPECT_EQ(page.slice(), "second page");
  EXPECT_FALSE(queue.take(page, 999999, 0));  // taken once

  // Page 3 carried no text (a prerendered body): it queues with an empty slice.
  queue.frameShown(ce::EPISODE_DWELL_MS, 4, 60000);
  ASSERT_TRUE(queue.take(page, 60000 + ce::DWELL_SETTLE_MS, ce::DWELL_SETTLE_MS));
  EXPECT_EQ(page.index, 3u);
  EXPECT_TRUE(page.slice().empty());
}

TEST(ConceptEngineDwell, SliceStopsAtCapOnCodePointBoundary) {
  EXPECT_EQ(ce::utf8Prefix("abc", 5), "abc");
  EXPECT_EQ(ce::utf8Prefix("a\xC3\xA9", 2), "a");
  EXPECT_EQ(ce::utf8Prefix("\xE4\xB8\x96\xE7\x95\x8C", 4), "\xE4\xB8\x96");

  ce::PageDwellQueue queue;
  auto& shown = queue.frameShown(0, 1, 0);
  const std::string word = "\xE4\xB8\x96\xE7\x95\x8C";  // 6 bytes, two code points
  size_t appended = 0;
  while (shown.append(word)) ++appended;
  EXPECT_EQ(appended, ce::DWELL_SLICE_BYTES / word.size());
  // 200 bytes hold 33 whole words; the 2 left are too few for the next code
  // point, so the 34th word adds nothing rather than a broken sequence.
  const auto slice = shown.slice();
  EXPECT_EQ(slice.size(), 33u * 6);
  EXPECT_EQ(slice.substr(slice.size() - 3), "\xE7\x95\x8C");
  // ASCII still fills the last 2 bytes ("xy"); then the slice is full.
  EXPECT_FALSE(shown.append("xyz"));
  EXPECT_EQ(shown.slice().size(), ce::DWELL_SLICE_BYTES);
}

TEST(ConceptEnginePositions, PacksEpubSpineAndPage) {
  EXPECT_EQ(ce::epubPosition(0, 0), 1u << 16);
  EXPECT_EQ(ce::epubPosition(2, 9), (3u << 16) | 9u);
  EXPECT_EQ(ce::epubPosition(-1, -4), 0u);
  ce::Match txt;
  txt.label = "notes";
  txt.page = 0;
  EXPECT_EQ(ce::describeMatch(txt), "notes p.1");
}

TEST(ConceptEngineHash, Fnv1a64KnownValues) {
  EXPECT_EQ(ce::fnv1a64(""), 0xcbf29ce484222325ULL);
  EXPECT_EQ(ce::fnv1a64("a"), 0xaf63dc4c8601ec8cULL);
  EXPECT_EQ(ce::fnv1a64("foobar"), 0x85944171f73967e8ULL);
}

// ---- Bigram Engram ------------------------------------------------------------------

TEST_F(ConceptEngineTest, ForwardAppliesBigramAcrossChunkBoundary) {
  const Shifts s;
  const std::string idiom = "一举两得";
  const char* texts[] = {"这真是一举两得的好办法", "他们一举两得，", "一举两得", "我们一举两得地完成了任务"};
  std::vector<uint8_t> model;
  std::string text;
  for (uint32_t seed = 1; seed < 200 && text.empty(); ++seed) {
    const auto candidate = makeModel(s, seed);
    for (const char* t : texts) {
      Spans spans;
      refForward(candidate, s, t, nullptr, 5, &spans);
      const size_t at = std::string(t).find(idiom);
      for (size_t c = 1; c < spans.size(); ++c) {
        if (spans[c - 1].first == at && spans[c].first == at + spans[c - 1].second &&
            spans[c - 1].second + spans[c].second == idiom.size()) {
          model = candidate;
          text = t;
        }
      }
      if (!text.empty()) break;
    }
  }
  ASSERT_FALSE(text.empty()) << "no seed splits the idiom into exactly two chunks";
  writeFile(modelPath, model);

  constexpr uint32_t slots = 4096;
  const uint64_t h2 = ce::fnv1a64(idiom);
  std::array<int8_t, 24> delta;
  delta.fill(40);
  const auto engram = makeEngram(slots, {{h2, delta, "kill two birds"}});
  writeFile(engramPath, engram);
  // Same slot, different span hash: the bigram must not fire.
  const auto stale = makeEngram(slots, {{h2 + slots, delta, "kill two birds"}});
  const std::string stalePath = (dir / "engram-stale.bin").string();
  writeFile(stalePath, stale);

  const auto with = encode(text, engramPath.c_str());
  const auto without = encode(text, stalePath.c_str());
  EXPECT_EQ(with, refForward(model, s, text, &engram));
  EXPECT_EQ(without, refForward(model, s, text, &stale));
  EXPECT_NE(with, without);
}

// ---- CS-PW v2: Chrono-CfC kernel -------------------------------------------------------

namespace {

constexpr size_t CS_D = 192, CS_KV = 96, CS_HD = 48;
constexpr size_t OFF_UP = 512, OFF_Q = 25088, OFF_K = 61952, OFF_V = 80384, OFF_O = 98816, OFF_DOWN = 135680,
                 OFF_SAL_W = 160256, OFF_SAL_B = 160384, OFF_GATE = 160512, OFF_FRAME_BANK = 226048,
                 OFF_FRAME_STR = 258816, OFF_PHRASE_BANK = 275200, OFF_PHRASE_STR = 340736, OFF_TAIL = 391936,
                 CSPW_FILE_BYTES = 393728;

template <typename T>
void poke(std::vector<uint8_t>& out, const size_t at, const T value) {
  memcpy(out.data() + at, &value, sizeof(T));
}

void pokeText(std::vector<uint8_t>& out, const size_t at, const size_t cap, const std::string& text) {
  memcpy(out.data() + at, text.data(), std::min(text.size(), cap));
}

// A CSPW v2 file with zero weights and no frame/phrase strings.
std::vector<uint8_t> makeCspw(const int16_t salienceBias = 0) {
  std::vector<uint8_t> out(CSPW_FILE_BYTES, 0);
  memcpy(out.data(), "CSPW", 4);
  poke<uint16_t>(out, 4, 2);
  poke<uint32_t>(out, 8, 192);
  poke<uint32_t>(out, 12, 128);
  poke<uint32_t>(out, 16, 4);
  poke<uint32_t>(out, 20, 256);
  poke<uint32_t>(out, 24, 512);
  poke<int16_t>(out, OFF_SAL_B, salienceBias);
  return out;
}

// Pseudo-random weights in [-range, range]; every third frame and fifth
// phrase stays empty.
std::vector<uint8_t> makeRandomCspw(const uint32_t seed, const int16_t salienceBias, const int range = 16) {
  auto out = makeCspw(salienceBias);
  uint32_t state = seed;
  for (size_t at = OFF_UP; at < OFF_TAIL; ++at) {
    state = state * 1664525u + 1013904223u;
    const int v = static_cast<int>((state >> 16) % static_cast<uint32_t>(2 * range + 1)) - range;
    out[at] = static_cast<uint8_t>(static_cast<int8_t>(v));
  }
  memset(out.data() + OFF_SAL_B, 0, 128);
  poke<int16_t>(out, OFF_SAL_B, salienceBias);
  memset(out.data() + OFF_FRAME_STR, 0, OFF_PHRASE_BANK - OFF_FRAME_STR);
  memset(out.data() + OFF_PHRASE_STR, 0, OFF_TAIL - OFF_PHRASE_STR);
  for (size_t f = 0; f < 256; ++f)
    if (f % 3 != 0) pokeText(out, OFF_FRAME_STR + f * 64, 63, "frame " + std::to_string(f));
  for (size_t p = 0; p < 512; ++p)
    if (p % 5 != 0) pokeText(out, OFF_PHRASE_STR + p * 100, 99, "phrase " + std::to_string(p));
  return out;
}

int64_t floorDiv(const int64_t a, const int64_t b) {
  int64_t q = a / b;
  if (a % b != 0 && ((a < 0) != (b < 0))) --q;
  return q;
}

using IVec = std::vector<int64_t>;

IVec refProject(const IVec& x, const int8_t* w, const size_t out) {
  IVec y(out);
  for (size_t j = 0; j < out; ++j) {
    int64_t acc = 0;
    for (size_t i = 0; i < x.size(); ++i) acc += x[i] * w[i * out + j];
    y[j] = sat8(acc >> 7);
  }
  return y;
}

int64_t refDot(const int8_t* a, const IVec& b) {
  int64_t acc = 0;
  for (size_t i = 0; i < b.size(); ++i) acc += a[i] * b[i];
  return acc;
}

struct RefSlots {
  std::array<IVec, 4> slot;
  int g[4] = {};
  int tau[4] = {};
  int frame[4] = {-1, -1, -1, -1};
  int phrase = -1;
  int arc = -1;
};

// Literal transcription of the CS-PW kernel addendum §B, LUTs from their
// definitions.
RefSlots refChrono(const std::vector<uint8_t>& file, const std::vector<std::array<int8_t, 128>>& scenes,
                   const std::vector<uint32_t>& pages, const int8_t* echo) {
  const auto at = [&](const size_t off) { return reinterpret_cast<const int8_t*>(file.data() + off); };
  int64_t expLut[65], frac[16], neg[17];
  for (int t = 0; t < 64; ++t) expLut[t] = std::lround(256.0 * std::pow(2.0, -t / 8.0));
  expLut[64] = 0;
  for (int i = 0; i < 16; ++i) frac[i] = std::lround(32768.0 * std::pow(2.0, i / 16.0));
  for (int i = 0; i <= 16; ++i) neg[i] = std::lround(32768.0 * std::pow(2.0, -i / 16.0));

  const size_t M = scenes.size();
  std::vector<IVec> s(M), h(M), q(M), k(M), v(M), r(M);
  std::array<std::vector<IVec>, 4> u;
  for (size_t m = 0; m < M; ++m) {
    s[m].assign(scenes[m].begin(), scenes[m].end());
    h[m] = refProject(s[m], at(OFF_UP), CS_D);
    q[m] = refProject(h[m], at(OFF_Q), CS_D);
    k[m] = refProject(h[m], at(OFF_K), CS_KV);
    v[m] = refProject(h[m], at(OFF_V), CS_KV);
  }
  RefSlots out;
  int16_t bias;
  memcpy(&bias, file.data() + OFF_SAL_B, 2);
  for (size_t a = 0; a < M; ++a) {
    IVec attn(CS_D);
    for (size_t head = 0; head < 4; ++head) {
      const size_t kv = head >> 1;
      std::vector<int64_t> score(a + 1), e(a + 1);
      for (size_t b = 0; b <= a; ++b) {
        int64_t acc = 0;
        for (size_t d = 0; d < CS_HD; ++d) acc += q[a][head * CS_HD + d] * k[b][kv * CS_HD + d];
        score[b] = acc >> 6;
      }
      const int64_t best = *std::max_element(score.begin(), score.end());
      int64_t S = 0;
      for (size_t b = 0; b <= a; ++b) S += e[b] = expLut[std::min<int64_t>(best - score[b], 64)];
      for (size_t d = 0; d < CS_HD; ++d) {
        int64_t acc = 0;
        for (size_t b = 0; b <= a; ++b) acc += floorDiv(e[b] * 256 + S / 2, S) * v[b][kv * CS_HD + d];
        attn[head * CS_HD + d] = sat8(acc >> 8);
      }
    }
    for (size_t j = 0; j < CS_D; ++j) {
      int64_t acc = 0;
      for (size_t i = 0; i < CS_D; ++i) acc += attn[i] * at(OFF_O)[i * CS_D + j];
      h[a][j] = sat8(h[a][j] + (acc >> 7));
    }
    const IVec v128 = refProject(h[a], at(OFF_DOWN), 128);
    r[a].resize(128);
    for (size_t j = 0; j < 128; ++j) r[a][j] = sat8((s[a][j] + v128[j]) >> 1);
    out.g[a] = static_cast<int>(std::clamp<int64_t>((refDot(at(OFF_SAL_W), r[a]) >> 7) + bias + 128, 0, 255));
    const int code = 64 + (out.g[a] >> 1);
    out.tau[a] = static_cast<int>(std::max<int64_t>(16, (frac[code & 15] * (int64_t{1} << (code >> 4)) + 16384) >> 15));
    for (size_t slot = 0; slot < 4; ++slot) {
      IVec y(128);
      for (size_t d = 0; d < 128; ++d) y[d] = sat8(refDot(at(OFF_GATE + slot * 128 * 128 + d * 128), r[a]) >> 7);
      u[slot].push_back(y);
    }
  }
  out.slot[0].assign(128, 0);
  int64_t den = 0;
  for (size_t m = 0; m < M; ++m) den += out.g[m] + 32;
  for (size_t d = 0; d < 128; ++d) {
    int64_t num = 0;
    for (size_t m = 0; m < M; ++m) num += (out.g[m] + 32) * u[0][m][d];
    out.slot[0][d] = sat8(floorDiv(2 * num + den, 2 * den));
  }
  out.slot[1] = u[1][0];
  IVec arc = u[2][0];
  for (size_t m = 1; m < M; ++m) {
    const int64_t dp = std::max<int64_t>(0, int64_t{pages[m]} - int64_t{pages[m - 1]});
    const int64_t x = dp * 256 / out.tau[m];
    int64_t f = 0;
    if (x < 4096) {
      const int64_t ip = x >> 8, xf = x & 255, seg = xf >> 4, t = xf & 15;
      const int64_t base = (neg[seg] * (16 - t) + neg[seg + 1] * t + 8) >> 4;
      f = ip == 0 ? base : (base + (int64_t{1} << (ip - 1))) >> ip;
    }
    const int64_t alpha = std::clamp<int64_t>((32768 - f) + ((out.g[m] * f) >> 9), 4096, 32768);
    for (size_t d = 0; d < 128; ++d) arc[d] = sat8(((32768 - alpha) * arc[d] + alpha * u[2][m][d] + 16384) >> 15);
  }
  out.slot[2] = arc;
  out.slot[3] = u[3][M - 1];
  if (echo)
    for (size_t d = 0; d < 128; ++d) out.slot[3][d] = sat8((out.slot[3][d] + echo[d]) >> 1);
  for (size_t g = 0; g < 4; ++g) {
    int64_t best = 0;
    for (size_t f = g * 64; f < g * 64 + 64; ++f) {
      if (file[OFF_FRAME_STR + f * 64] == 0) continue;
      const int64_t score = refDot(at(OFF_FRAME_BANK + f * 128), out.slot[g]);
      if (out.frame[g] < 0 || score > best) {
        out.frame[g] = static_cast<int>(f);
        best = score;
      }
    }
  }
  int64_t bestPhrase = 0;
  for (size_t p = 0; p < 512; ++p) {
    if (file[OFF_PHRASE_STR + p * 100] == 0) continue;
    const int64_t score = refDot(at(OFF_PHRASE_BANK + p * 128), out.slot[0]);
    if (out.phrase < 0 || score > bestPhrase) {
      out.phrase = static_cast<int>(p);
      bestPhrase = score;
    }
  }
  int64_t bestArc = 0;
  for (size_t m = 1; m < M; ++m) {
    int64_t score = 0;
    for (size_t d = 0; d < 128; ++d) score += arc[d] * u[2][m][d];
    if (out.arc < 0 || score >= bestArc) {
      out.arc = static_cast<int>(m);
      bestArc = score;
    }
  }
  return out;
}

std::array<int8_t, 128> randomVec(uint32_t& state, const int range = 100) {
  std::array<int8_t, 128> v{};
  for (auto& x : v) {
    state = state * 1664525u + 1013904223u;
    x = static_cast<int8_t>(static_cast<int>((state >> 16) % static_cast<uint32_t>(2 * range + 1)) - range);
  }
  return v;
}

void expectSlotsMatch(const ce::ChronoSlots& got, const RefSlots& want, const size_t M, const std::string& what) {
  for (size_t m = 0; m < M; ++m) {
    EXPECT_EQ(got.salience[m], want.g[m]) << what << " scene " << m;
    EXPECT_EQ(got.tau[m], want.tau[m]) << what << " scene " << m;
  }
  for (size_t k = 0; k < 4; ++k) {
    for (size_t d = 0; d < 128; ++d) ASSERT_EQ(got.slot[k][d], want.slot[k][d]) << what << " slot " << k << " d " << d;
    EXPECT_EQ(got.frame[k], want.frame[k]) << what << " slot " << k;
  }
  EXPECT_EQ(got.phrase, want.phrase) << what;
  EXPECT_EQ(got.arcScene, want.arc) << what;
}

}  // namespace

TEST(ConceptEngineCspw, HalfLifeAndDecayFollowTheirDefinitions) {
  for (int g = 0; g < 256; ++g) {
    const double exact = std::pow(2.0, (64 + (g >> 1)) / 16.0);
    const uint16_t tau = ce::cspwHalfLife(static_cast<uint8_t>(g));
    EXPECT_NEAR(tau, std::max(16.0, exact), 0.51) << g;
  }
  EXPECT_EQ(ce::cspwHalfLife(0), 16);
  EXPECT_EQ(ce::cspwHalfLife(255), 3922);
  EXPECT_EQ(ce::cspwDecayQ15(0, 16), 32768);
  EXPECT_EQ(ce::cspwDecayQ15(16, 16), 16384);
  EXPECT_EQ(ce::cspwDecayQ15(32, 16), 8192);
  EXPECT_EQ(ce::cspwDecayQ15(256, 16), 0);
  for (const uint32_t tau : {16u, 45u, 300u, 3922u}) {
    uint16_t previous = 32768;
    for (uint32_t dp = 0; dp < 16 * tau; dp += std::max(1u, tau / 37)) {
      const uint16_t f = ce::cspwDecayQ15(dp, tau);
      EXPECT_NEAR(f, 32768.0 * std::pow(2.0, -static_cast<double>(dp) / tau), 160.0) << dp << "/" << tau;
      EXPECT_LE(f, previous) << dp << "/" << tau;
      previous = f;
    }
  }
}

TEST_F(ConceptEngineTest, ChronoSlotsMatchLiteralReferenceBitExact) {
  const std::vector<std::vector<uint32_t>> pageSets = {{7}, {3, 10}, {3, 10, 200}, {1, 2, 60, 59}, {5, 5, 5, 4000}};
  uint32_t state = 99;
  for (uint32_t seed = 1; seed <= 4; ++seed) {
    const int16_t bias = static_cast<int16_t>(-160 + 90 * static_cast<int>(seed));
    const auto file = makeRandomCspw(seed, bias);
    writeFile(cspwPath, file);
    ce::unloadModel();
    for (const auto& pages : pageSets) {
      for (const bool withEcho : {false, true}) {
        std::vector<std::array<int8_t, 128>> scenes;
        int8_t packed[4][128];
        for (size_t m = 0; m < pages.size(); ++m) {
          scenes.push_back(randomVec(state));
          memcpy(packed[m], scenes.back().data(), 128);
        }
        const auto echo = randomVec(state);
        ce::ChronoSlots got;
        ASSERT_TRUE(ce::runChronoSlots(packed, pages.data(), pages.size(), withEcho ? echo.data() : nullptr, got));
        const RefSlots want = refChrono(file, scenes, pages, withEcho ? echo.data() : nullptr);
        expectSlotsMatch(got, want, pages.size(),
                         "seed " + std::to_string(seed) + " M " + std::to_string(pages.size()) +
                             (withEcho ? " echo" : ""));
      }
    }
  }
}

TEST_F(ConceptEngineTest, ChronoSlotsRejectBadInput) {
  int8_t scenes[4][128] = {};
  const uint32_t pages[4] = {};
  ce::ChronoSlots out;
  EXPECT_FALSE(ce::runChronoSlots(scenes, pages, 1, nullptr, out));  // no cspw.bin
  auto file = makeCspw();
  file[8] = 128;  // d_model
  writeFile(cspwPath, file);
  EXPECT_FALSE(ce::runChronoSlots(scenes, pages, 1, nullptr, out));
  writeFile(cspwPath, makeCspw());
  ce::unloadModel();
  EXPECT_FALSE(ce::runChronoSlots(scenes, pages, 0, nullptr, out));
  EXPECT_FALSE(ce::runChronoSlots(scenes, pages, 5, nullptr, out));
  EXPECT_TRUE(ce::runChronoSlots(scenes, pages, 4, nullptr, out));
  EXPECT_EQ(out.frame[0], -1);
  EXPECT_EQ(out.phrase, -1);
  EXPECT_EQ(out.arcScene, 3);  // all-zero scores tie toward the latest scene
}

// With only identity slot gates the slots show the temporal rules directly:
// Slot 1 keeps the origin, Slot 2 swaps to the latest scene after a long gap
// and keeps most of the old one after no gap, more so at low salience.
TEST_F(ConceptEngineTest, ChronoSlotsSwapArcByPageGapAndSalience) {
  int8_t scenes[2][128] = {};
  for (size_t d = 0; d < 64; ++d) scenes[0][d] = 100;
  for (size_t d = 64; d < 128; ++d) scenes[1][d] = 100;
  const auto run = [&](const int16_t bias, const uint32_t gap) {
    auto file = makeCspw(bias);
    for (size_t k = 0; k < 4; ++k)
      for (size_t d = 0; d < 128; ++d) file[OFF_GATE + k * 128 * 128 + d * 128 + d] = 127;
    writeFile(cspwPath, file);
    ce::unloadModel();
    const uint32_t pages[2] = {1, 1 + gap};
    ce::ChronoSlots out;
    EXPECT_TRUE(ce::runChronoSlots(scenes, pages, 2, nullptr, out));
    return out;
  };
  // r = 100 >> 1 = 50, u = (127 * 50) >> 7 = 49 on each scene's half.
  const auto farLow = run(-128, 399);
  EXPECT_EQ(farLow.salience[1], 0);
  EXPECT_EQ(farLow.tau[1], 16);
  EXPECT_EQ(farLow.slot[1][0], 49);
  EXPECT_EQ(farLow.slot[1][64], 0);
  EXPECT_EQ(farLow.slot[2][0], 0);
  EXPECT_EQ(farLow.slot[2][64], 49);
  EXPECT_EQ(farLow.slot[3][64], 49);
  EXPECT_EQ(farLow.slot[0][0], 25);  // (2 * 32 * 49 + 64) // 128

  const auto nearLow = run(-128, 0);  // alpha floors at 4096
  EXPECT_EQ(nearLow.slot[2][0], 43);
  EXPECT_EQ(nearLow.slot[2][64], 6);

  const auto nearHigh = run(127, 0);  // alpha = 255 * 32768 >> 9 = 16320
  EXPECT_EQ(nearHigh.salience[1], 255);
  EXPECT_EQ(nearHigh.tau[1], 3922);
  EXPECT_EQ(nearHigh.slot[2][0], 25);
  EXPECT_EQ(nearHigh.slot[2][64], 24);
  EXPECT_EQ(nearHigh.slot[1][0], 49);
}

// Trainer golden cases: CONCEPT_ENGINE_GOLDEN_DIR=<dir with cspw.bin, cspw_golden.bin>.
TEST_F(ConceptEngineTest, MatchesCspwGoldenVectors) {
  const char* goldenDir = std::getenv("CONCEPT_ENGINE_GOLDEN_DIR");
  if (!goldenDir || !*goldenDir) GTEST_SKIP() << "CONCEPT_ENGINE_GOLDEN_DIR not set";
  const fs::path golden(goldenDir);
  if (!fs::exists(golden / "cspw_golden.bin")) GTEST_SKIP() << "no cspw_golden.bin";
  goldenCspw = (golden / "cspw.bin").string();
  ce::Paths paths = scratchPaths();
  paths.cspw = goldenCspw.c_str();
  ce::setPaths(paths);
  std::ifstream in(golden / "cspw_golden.bin", std::ios::binary);
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  ASSERT_GE(bytes.size(), 16u);
  ASSERT_EQ(memcmp(bytes.data(), "CSGV", 4), 0);
  uint32_t version, cases;
  memcpy(&version, bytes.data() + 4, 4);
  memcpy(&cases, bytes.data() + 8, 4);
  ASSERT_EQ(version, 1u);
  constexpr size_t CASE_BYTES = 1196;
  ASSERT_EQ(bytes.size(), 16 + cases * CASE_BYTES);
  for (uint32_t c = 0; c < cases; ++c) {
    const uint8_t* p = bytes.data() + 16 + c * CASE_BYTES;
    const size_t M = p[0];
    uint32_t pages[4];
    memcpy(pages, p + 4, 16);
    int8_t scenes[4][128];
    memcpy(scenes, p + 20, 512);
    const auto* echo = reinterpret_cast<const int8_t*>(p + 532);
    ce::ChronoSlots got;
    ASSERT_TRUE(ce::runChronoSlots(scenes, pages, M, p[1] ? echo : nullptr, got)) << "case " << c;
    for (size_t m = 0; m < M; ++m) {
      uint16_t tau;
      memcpy(&tau, p + 664 + m * 2, 2);
      EXPECT_EQ(got.salience[m], p[660 + m]) << "case " << c << " scene " << m;
      EXPECT_EQ(got.tau[m], tau) << "case " << c << " scene " << m;
    }
    EXPECT_EQ(memcmp(got.slot, p + 672, 512), 0) << "case " << c;
    int16_t frame[4], phrase, arc;
    memcpy(frame, p + 1184, 8);
    memcpy(&phrase, p + 1192, 2);
    memcpy(&arc, p + 1194, 2);
    for (size_t k = 0; k < 4; ++k) EXPECT_EQ(got.frame[k], frame[k]) << "case " << c << " slot " << k;
    EXPECT_EQ(got.phrase, phrase) << "case " << c;
    EXPECT_EQ(got.arcScene, arc) << "case " << c;
  }
  EXPECT_GT(cases, 0u);
}

// ---- CS-PW v2: spoiler-gated story refreshers --------------------------------------------

namespace {

struct SgEvent {
  uint32_t cutoff;
  uint16_t page;
  uint8_t hint;
  uint8_t salience;
  std::string gloss;
  std::string target;
};

struct SgEntity {
  std::string term;
  std::string book;  // empty = any book
  uint32_t firstCutoff;
  std::string name;
  std::vector<SgEvent> events;
};

struct SgAlias {
  std::string alias;
  std::string canonical;
  uint32_t reveal;
};

size_t align512(const size_t n) { return (n + 511) / 512 * 512; }

std::vector<uint8_t> makeStoryGraph(std::vector<SgEntity> entities, std::vector<SgAlias> aliases) {
  const auto key = [](const SgEntity& e) {
    return std::make_pair(ce::fnv1a64(e.term), e.book.empty() ? uint64_t{0} : ce::fnv1a64(e.book));
  };
  std::sort(entities.begin(), entities.end(), [&](const SgEntity& a, const SgEntity& b) { return key(a) < key(b); });
  std::sort(aliases.begin(), aliases.end(),
            [](const SgAlias& a, const SgAlias& b) { return ce::fnv1a64(a.alias) < ce::fnv1a64(b.alias); });
  size_t events = 0;
  for (const auto& e : entities) events += e.events.size();
  const size_t entitiesOffset = 512;
  const size_t eventsOffset = align512(entitiesOffset + entities.size() * 64);
  const size_t aliasesOffset = align512(eventsOffset + events * 256);
  std::vector<uint8_t> out(aliasesOffset + aliases.size() * 24, 0);
  memcpy(out.data(), "SGRF", 4);
  poke<uint16_t>(out, 4, 1);
  poke<uint32_t>(out, 8, static_cast<uint32_t>(entities.size()));
  poke<uint32_t>(out, 12, static_cast<uint32_t>(events));
  poke<uint32_t>(out, 16, static_cast<uint32_t>(aliases.size()));
  poke<uint32_t>(out, 20, static_cast<uint32_t>(entitiesOffset));
  poke<uint32_t>(out, 24, static_cast<uint32_t>(eventsOffset));
  poke<uint32_t>(out, 28, static_cast<uint32_t>(aliasesOffset));
  uint32_t next = 0;
  for (size_t i = 0; i < entities.size(); ++i) {
    const auto& e = entities[i];
    const size_t at = entitiesOffset + i * 64;
    poke<uint64_t>(out, at, key(e).first);
    poke<uint64_t>(out, at + 8, key(e).second);
    poke<uint32_t>(out, at + 16, e.firstCutoff);
    poke<uint32_t>(out, at + 20, next);
    poke<uint16_t>(out, at + 24, static_cast<uint16_t>(e.events.size()));
    pokeText(out, at + 28, 32, e.name);
    for (const auto& ev : e.events) {
      const size_t eat = eventsOffset + static_cast<size_t>(next++) * 256;
      poke<uint32_t>(out, eat, ev.cutoff);
      poke<uint16_t>(out, eat + 4, ev.page);
      out[eat + 6] = ev.hint;
      out[eat + 7] = ev.salience;
      pokeText(out, eat + 8, 64, ev.gloss);
      pokeText(out, eat + 72, 56, ev.target);
      for (size_t d = 0; d < 128; ++d)
        out[eat + 128 + d] = static_cast<uint8_t>(static_cast<int8_t>((d * 7 + next) % 41 - 20));
    }
  }
  for (size_t i = 0; i < aliases.size(); ++i) {
    const size_t at = aliasesOffset + i * 24;
    poke<uint64_t>(out, at, ce::fnv1a64(aliases[i].alias));
    poke<uint64_t>(out, at + 8, ce::fnv1a64(aliases[i].canonical));
    poke<uint32_t>(out, at + 16, aliases[i].reveal);
  }
  return out;
}

// Chapter n of a TXT novel stored at 10 KiB a chapter.
uint32_t chapter(const int n) { return ce::txtStoryCutoff(static_cast<size_t>(n) * 10240); }

const char* const DOUPO = "/books/Chinese/斗破苍穹.txt";

std::vector<uint8_t> makeDoupoGraph() {
  return makeStoryGraph(
      {
          {"萧炎",
           DOUPO,
           chapter(1),
           "Xiao Yan (Dou Qi disciple)",
           {
               {chapter(1), 1, 1, 200, "introduced at Xiao Clan test (Ch.1)", "Xiao Clan"},
               {chapter(2), 2, 0, 50, "shamed at the Dou Qi test (Ch.2)", "Xiao Clan"},
               {chapter(3), 3, 2, 180, "clashed over 3-year pact (Ch.3)", "Wu Tan City"},
               {chapter(5), 5, 3, 90, "rise-from-ruin arc", "Martial Peak: fallen genius returns"},
               {chapter(500), 500, 2, 255, "crowned Yan Di, the Flame Emperor (Ch.500)", "Central Plains"},
           }},
          {"萧炎", "", 0, "Xiao Yan (any book)", {{0, 0, 1, 10, "a Dou Qi cultivator", ""}}},
          {"乌坦城",
           DOUPO,
           chapter(3),
           "Wu Tan City (Xiao Clan seat)",
           {{chapter(3), 3, 1, 100, "Xiao Clan's home city (Ch.3)", ""}}},
          {"美杜莎",
           DOUPO,
           chapter(300),
           "Medusa (Snake-People queen)",
           {{chapter(300), 300, 1, 220, "Snake-People queen met in the desert (Ch.300)", "Tagoo Desert"}}},
          {"药老", DOUPO, chapter(1), "", {{chapter(1), 1, 1, 10, "ring spirit in Xiao Yan's ring (Ch.1)", ""}}},
          {"纳兰嫣然", DOUPO, chapter(2), "Nalan Yanran", {{chapter(3), 3, 1, 60, "broke the engagement (Ch.3)", ""}}},
      },
      {{"炎帝", "萧炎", chapter(500)}});
}

std::string weave(const std::string& word, const char* book, const uint32_t cutoff, const size_t cap = 512) {
  std::vector<char> out(cap, '\x7f');
  if (!ce::weaveEntityRefresher(word.data(), word.size(), book, cutoff, out.data(), out.size())) return "<none>";
  return out.data();
}

}  // namespace

TEST(ConceptEngineCutoffs, PackLayoutIndependentPositions) {
  EXPECT_EQ(ce::epubStoryCutoff(0, 0), 1u << 16);
  EXPECT_EQ(ce::epubStoryCutoff(2, 64 * 7 + 63), (3u << 16) | 7u);
  EXPECT_EQ(ce::epubStoryCutoff(1, 0xFFFFFFFFu), (2u << 16) | 0xFFFFu);
  EXPECT_EQ(ce::epubStoryCutoff(-1, 640), 10u);
  EXPECT_EQ(ce::txtStoryCutoff(0), 1u << 16);
  EXPECT_EQ(ce::txtStoryCutoff(5 * 1024 + 1023), (1u << 16) | 5u);
  EXPECT_EQ(ce::txtStoryCutoff(size_t{1} << 40), (1u << 16) | 0xFFFFu);
  EXPECT_LT(ce::epubStoryCutoff(3, 1 << 20), ce::epubStoryCutoff(4, 0));
}

TEST_F(ConceptEngineTest, StoryRefresherUsesOnlyEventsUpToCutoff) {
  writeFile(storyPath, makeDoupoGraph());
  EXPECT_EQ(weave("萧炎", DOUPO, chapter(1)),
            "Story: Xiao Yan (Dou Qi disciple) — introduced at Xiao Clan test (Ch.1).");
  const std::string ch4 = weave("萧炎", DOUPO, chapter(4));
  EXPECT_EQ(ch4,
            "Story: Xiao Yan (Dou Qi disciple) — introduced at Xiao Clan test (Ch.1); recently clashed over 3-year "
            "pact (Ch.3).");
  EXPECT_EQ(ch4.find("Flame Emperor"), std::string::npos);
  EXPECT_EQ(ch4.find("Martial Peak"), std::string::npos);
  EXPECT_EQ(weave("萧炎", DOUPO, chapter(6)), ch4 + " | Echo: Martial Peak: fallen genius returns");
}

TEST_F(ConceptEngineTest, StoryRefresherGatesEntitiesAndAliases) {
  writeFile(storyPath, makeDoupoGraph());
  // Not introduced yet: no name, no refresher.
  EXPECT_EQ(weave("美杜莎", DOUPO, chapter(4)), "<none>");
  EXPECT_EQ(weave("美杜莎", DOUPO, chapter(300)),
            "Story: Medusa (Snake-People queen) — Snake-People queen met in the desert (Ch.300).");
  EXPECT_EQ(weave("萧炎", DOUPO, chapter(0)), "<none>");
  // Introduced, but nothing has happened yet.
  EXPECT_EQ(weave("纳兰嫣然", DOUPO, chapter(2)), "<none>");
  // 炎帝 only becomes Xiao Yan once the reader reaches the reveal.
  EXPECT_EQ(weave("炎帝", DOUPO, chapter(1)), "<none>");
  EXPECT_EQ(weave("炎帝", DOUPO, chapter(499)), "<none>");
  const std::string revealed =
      "Story: Xiao Yan (Dou Qi disciple) — introduced at Xiao Clan test (Ch.1); recently crowned Yan Di, the Flame "
      "Emperor (Ch.500). | Echo: Martial Peak: fallen genius returns";
  EXPECT_EQ(weave("炎帝", DOUPO, chapter(500)), revealed);
  EXPECT_EQ(weave("萧炎", DOUPO, 0), revealed);  // 0 = ungated
  EXPECT_EQ(weave("萧火火", DOUPO, 0), "<none>");
}

TEST_F(ConceptEngineTest, StoryRefresherPrefersBookRecordOverWildcard) {
  writeFile(storyPath, makeDoupoGraph());
  EXPECT_EQ(weave("萧炎", "/books/other.epub", ce::epubStoryCutoff(0, 0)),
            "Story: Xiao Yan (any book) — a Dou Qi cultivator.");
  EXPECT_EQ(weave("萧炎", nullptr, 0), "Story: Xiao Yan (any book) — a Dou Qi cultivator.");
  EXPECT_EQ(weave("乌坦城", "/books/other.epub", 0), "<none>");
  EXPECT_EQ(weave("乌坦城", DOUPO, chapter(4)), "Story: Wu Tan City (Xiao Clan seat) — Xiao Clan's home city (Ch.3).");
}

TEST_F(ConceptEngineTest, StoryRefresherFallsBackToWordAndCutsOnCodepoints) {
  writeFile(storyPath, makeDoupoGraph());
  EXPECT_EQ(weave("药老", DOUPO, chapter(1)), "Story: 药老 — ring spirit in Xiao Yan's ring (Ch.1).");
  EXPECT_EQ(weave("药老", DOUPO, chapter(1), 12), "Story: 药");  // 11 bytes allowed, 老 would end at 13
  EXPECT_EQ(weave("药老", DOUPO, chapter(1), 14), "Story: 药老");
  EXPECT_EQ(weave("萧炎", DOUPO, chapter(1), 0), "<none>");
}

TEST_F(ConceptEngineTest, StoryRefresherRejectsMissingOrForeignGraph) {
  EXPECT_EQ(weave("萧炎", DOUPO, chapter(4)), "<none>");
  auto graph = makeDoupoGraph();
  graph[0] = 'X';
  writeFile(storyPath, graph);
  EXPECT_EQ(weave("萧炎", DOUPO, chapter(4)), "<none>");
  graph = makeDoupoGraph();
  graph.resize(graph.size() - 10);  // alias table cut short
  writeFile(storyPath, graph);
  EXPECT_EQ(weave("萧炎", DOUPO, chapter(4)), "<none>");
}

TEST_F(ConceptEngineTest, StoryRefresherWeavesCspwFrames) {
  writeFile(storyPath, makeDoupoGraph());
  auto file = makeCspw();
  pokeText(file, OFF_FRAME_STR + 0 * 64, 63, "%E, %R");
  pokeText(file, OFF_FRAME_STR + 64 * 64, 63, "first %O, at %L");
  pokeText(file, OFF_FRAME_STR + 128 * 64, 63, "lately %A in %L");
  pokeText(file, OFF_FRAME_STR + 192 * 64, 63, "cf. %B (100%%)");
  pokeText(file, OFF_PHRASE_STR + 0 * 100, 99, "Dou Qi prodigy");
  writeFile(cspwPath, file);
  EXPECT_EQ(weave("萧炎", DOUPO, chapter(6)),
            "Story: Xiao Yan (Dou Qi disciple), Dou Qi prodigy — first introduced at Xiao Clan test (Ch.1), at Xiao "
            "Clan; lately clashed over 3-year pact (Ch.3) in Wu Tan City. | Echo: cf. Martial Peak: fallen genius "
            "returns (100%)");
  // An empty placeholder (%L without a place) falls back to the part default.
  EXPECT_EQ(weave("乌坦城", DOUPO, chapter(4)),
            "Story: Wu Tan City (Xiao Clan seat), Dou Qi prodigy — Xiao Clan's home city (Ch.3).");
}

// ---- PE-Writer margin riffs ----------------------------------------------------------------

namespace {

constexpr size_t PE_VOCAB = 2048;
constexpr size_t PE_STEPS = 3;
constexpr size_t PE_TOKENS = 36;
constexpr size_t PE_BUCKETS = 64;
constexpr size_t PE_SLOTS = 64;

struct PeLayer {
  int s = 7;
  std::vector<int16_t> bias;
  std::vector<int8_t> w;  // [out][in]
};

struct PeModel {
  std::vector<int8_t> step, centroids, tok, pos;
  PeLayer fc1, fc2, gate, dw, outp, lm, copy;
  std::vector<std::string> vocab;  // 2048 entries, at most 16 bytes each (16 = no NUL)
};

struct ClauseSlot {
  std::array<int8_t, 128> vec{};
  std::array<uint16_t, 16> toks{};
  std::string text;  // empty = invalid slot
};

using ClauseBank = std::vector<ClauseSlot>;  // bucket * 64 + slot

struct Lcg {
  uint32_t state;
  int8_t next(const int range) {
    state = state * 1664525u + 1013904223u;
    return static_cast<int8_t>(static_cast<int>((state >> 8) % static_cast<uint32_t>(2 * range + 1)) - range);
  }
  uint32_t raw() {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
  }
};

std::vector<int8_t> randomBytes(Lcg& rng, const size_t n, const int range) {
  std::vector<int8_t> out(n);
  for (auto& v : out) v = rng.next(range);
  return out;
}

PeLayer randomLayer(Lcg& rng, const size_t out, const size_t in, const int s, const int range, const int biasRange) {
  PeLayer layer;
  layer.s = s;
  layer.w = randomBytes(rng, out * in, range);
  layer.bias.resize(out);
  for (auto& b : layer.bias) b = static_cast<int16_t>(rng.next(127) * biasRange / 127);
  return layer;
}

PeModel makePeModel(const uint32_t seed) {
  Lcg rng{seed};
  PeModel m;
  m.step = randomBytes(rng, PE_STEPS * 128, 60);
  m.fc1 = randomLayer(rng, 128, 128, 8, 40, 16);
  m.fc2 = randomLayer(rng, 128, 128, 7, 40, 16);
  m.centroids = randomBytes(rng, PE_BUCKETS * 128, 90);
  m.tok = randomBytes(rng, PE_VOCAB * 128, 80);
  m.pos = randomBytes(rng, PE_TOKENS * 128, 40);
  m.gate = randomLayer(rng, 128, 128, 9, 50, 32);
  m.dw = randomLayer(rng, 128, 3, 6, 100, 8);
  m.outp = randomLayer(rng, 128, 128, 8, 50, 8);
  m.lm = randomLayer(rng, PE_VOCAB, 128, 9, 60, 200);
  m.copy = randomLayer(rng, 1, 128, 9, 50, 16);
  m.vocab.resize(PE_VOCAB);
  for (size_t v = 8; v < PE_VOCAB; ++v) m.vocab[v] = "w" + std::to_string(v);
  m.vocab[8] = "reveals";
  m.vocab[9] = ",";
  m.vocab[10] = "##ly";
  m.vocab[11] = "(";
  m.vocab[12] = "beneath";
  m.vocab[13] = "。";
  m.vocab[14] = "暗流";
  m.vocab[15] = "'s";
  m.vocab[16] = "counterbalances,";  // fills all 16 bytes
  return m;
}

void putLayer(std::vector<uint8_t>& out, const PeLayer& layer) {
  put<float>(out, std::ldexp(1.0f, -layer.s));
  for (const int16_t b : layer.bias) put<int16_t>(out, b);
  out.insert(out.end(), layer.w.begin(), layer.w.end());
}

std::vector<uint8_t> serializePe(const PeModel& m) {
  std::vector<uint8_t> out;
  put<uint32_t>(out, 0x52574550);
  for (const uint16_t v : {1, 128, 128, 3, 2048, 36, 64, 64}) put<uint16_t>(out, v);
  out.resize(32, 0);
  out.insert(out.end(), m.step.begin(), m.step.end());
  putLayer(out, m.fc1);
  putLayer(out, m.fc2);
  out.insert(out.end(), m.centroids.begin(), m.centroids.end());
  out.insert(out.end(), m.tok.begin(), m.tok.end());
  out.insert(out.end(), m.pos.begin(), m.pos.end());
  putLayer(out, m.gate);
  putLayer(out, m.dw);
  putLayer(out, m.outp);
  putLayer(out, m.lm);
  putLayer(out, m.copy);
  for (const auto& word : m.vocab) {
    char field[16] = {};
    memcpy(field, word.data(), std::min<size_t>(word.size(), 16));
    out.insert(out.end(), field, field + 16);
  }
  EXPECT_EQ(out.size(), 641726u);
  return out;
}

// Every third slot is empty, bucket 5 has no valid slot at all, and some token
// ids are out of range (they must read as 0).
ClauseBank makeClauseBank(const uint32_t seed) {
  Lcg rng{seed};
  ClauseBank bank(PE_BUCKETS * PE_SLOTS);
  for (size_t b = 0; b < PE_BUCKETS; ++b) {
    for (size_t j = 0; j < PE_SLOTS; ++j) {
      ClauseSlot& slot = bank[b * PE_SLOTS + j];
      for (auto& v : slot.vec) v = rng.next(100);
      for (size_t k = 0; k < 16; ++k) {
        const uint32_t r = rng.raw();
        slot.toks[k] = k >= 12 ? 0 : (r % 9 == 0 ? static_cast<uint16_t>(3000 + r % 100) : 8 + r % (PE_VOCAB - 8));
      }
      if (b != 5 && j % 3 != 2) slot.text = "b" + std::to_string(b) + "s" + std::to_string(j) + " clause";
    }
  }
  return bank;
}

std::vector<uint8_t> serializeClauses(const ClauseBank& bank) {
  std::vector<uint8_t> out;
  put<uint32_t>(out, 0x534C4357);
  for (const uint16_t v : {1, 64, 64, 256}) put<uint16_t>(out, v);
  out.resize(32, 0);
  for (const ClauseSlot& slot : bank) {
    uint8_t raw[256] = {};
    memcpy(raw, slot.vec.data(), 128);
    memcpy(raw + 128, slot.toks.data(), 32);
    raw[160] = 0;
    raw[161] = static_cast<uint8_t>(std::min<size_t>(slot.text.size(), 93));
    memcpy(raw + 162, slot.text.data(), std::min<size_t>(slot.text.size(), 93));
    out.insert(out.end(), raw, raw + 256);
  }
  EXPECT_EQ(out.size(), 1048608u);
  return out;
}

// ---- Literal reference of pe-writer-kernel-addendum.md --------------------------------------

int64_t refSh(const int64_t a, const int s) { return s > 0 ? (a + (int64_t{1} << (s - 1))) >> s : a; }
int refSat8(const int64_t a) { return static_cast<int>(std::clamp<int64_t>(a, -127, 127)); }

std::vector<int64_t> refLin(const PeLayer& layer, const std::vector<int>& x) {
  const size_t in = x.size();
  const size_t out = layer.bias.size();
  std::vector<int64_t> y(out);
  for (size_t o = 0; o < out; ++o) {
    int64_t acc = 0;
    for (size_t i = 0; i < in; ++i) acc += static_cast<int64_t>(layer.w[o * in + i]) * x[i];
    y[o] = refSh(acc, layer.s) + layer.bias[o];
  }
  return y;
}

std::vector<int> refNormalize(const std::vector<int64_t>& r) {
  int64_t n2 = 0;
  for (const int64_t v : r) n2 += v * v;
  int64_t n = static_cast<int64_t>(std::sqrt(static_cast<long double>(n2)));
  while (n * n > n2) --n;
  while ((n + 1) * (n + 1) <= n2) ++n;
  std::vector<int> out(r.size());
  for (size_t d = 0; d < r.size(); ++d) {
    if (n == 0) continue;
    const int64_t a = 127 * r[d];
    out[d] = refSat8(a >= 0 ? (a + n / 2) / n : -((-a + n / 2) / n));
  }
  return out;
}

int64_t refDot(const std::vector<int>& a, const int8_t* b) {
  int64_t sum = 0;
  for (size_t d = 0; d < 128; ++d) sum += a[d] * static_cast<int64_t>(b[d]);
  return sum;
}

struct PeExpect {
  int8_t plan[PE_STEPS][128] = {};
  uint8_t buckets[PE_STEPS] = {};
  int clauseSlot[PE_STEPS] = {-1, -1, -1};
  uint16_t tokens[PE_TOKENS] = {};
  std::string text;
};

bool refCjk(const std::string& s, size_t i, size_t end) {
  const auto b = static_cast<uint8_t>(s[i]);
  size_t n = b < 0x80 ? 1 : (b >= 0xC0 && b < 0xE0) ? 2 : (b >= 0xE0 && b < 0xF0) ? 3 : (b >= 0xF0 && b < 0xF8) ? 4 : 0;
  if (n == 0 || i + n > s.size() || (end != 0 && i + n != end)) return false;
  uint32_t cp = n == 1 ? b : n == 2 ? (b & 0x1F) : n == 3 ? (b & 0x0F) : (b & 0x07);
  for (size_t k = 1; k < n; ++k) {
    const auto c = static_cast<uint8_t>(s[i + k]);
    if ((c & 0xC0) != 0x80) return false;
    cp = (cp << 6) | (c & 0x3F);
  }
  return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

PeExpect referenceRiff(const PeModel& m, const ClauseBank& bank, const int8_t* cPage, const int8_t* cEcho,
                       const std::string& subject, const std::string& role, const std::string& echo) {
  PeExpect e;
  std::vector<int> prev(cPage, cPage + 128);
  std::vector<std::vector<int>> plan;
  for (size_t s = 0; s < PE_STEPS; ++s) {
    std::vector<int> h(128);
    for (size_t d = 0; d < 128; ++d) h[d] = refSat8((prev[d] + (cEcho ? cEcho[d] : 0) + m.step[s * 128 + d]) >> 1);
    for (int k = 0; k < 2; ++k) {
      const auto a64 = refLin(m.fc1, h);
      std::vector<int> a(128);
      for (size_t d = 0; d < 128; ++d) a[d] = static_cast<int>(std::clamp<int64_t>(a64[d], 0, 127));
      const auto f = refLin(m.fc2, a);
      std::vector<int64_t> r(128);
      for (size_t d = 0; d < 128; ++d) r[d] = h[d] + f[d];
      h = refNormalize(r);
    }
    for (size_t d = 0; d < 128; ++d) e.plan[s][d] = static_cast<int8_t>(h[d]);
    plan.push_back(h);
    prev = h;
  }
  std::vector<int> r[PE_STEPS];
  std::array<uint16_t, 16> toks[PE_STEPS] = {};
  int sim[PE_STEPS] = {};
  std::string clauseText[PE_STEPS];
  for (size_t s = 0; s < PE_STEPS; ++s) {
    int64_t best = INT64_MIN;
    for (size_t b = 0; b < PE_BUCKETS; ++b) {
      const int64_t score = refDot(plan[s], m.centroids.data() + b * 128);
      if (score > best) {
        best = score;
        e.buckets[s] = static_cast<uint8_t>(b);
      }
    }
    r[s].assign(128, 0);
    best = INT64_MIN;
    for (size_t j = 0; j < PE_SLOTS; ++j) {
      const ClauseSlot& slot = bank[e.buckets[s] * PE_SLOTS + j];
      if (slot.text.empty()) continue;
      const int64_t score = refDot(plan[s], slot.vec.data());
      if (score > best) {
        best = score;
        e.clauseSlot[s] = static_cast<int>(j);
      }
    }
    if (e.clauseSlot[s] >= 0) {
      const ClauseSlot& slot = bank[e.buckets[s] * PE_SLOTS + e.clauseSlot[s]];
      r[s].assign(slot.vec.begin(), slot.vec.end());
      for (size_t k = 0; k < 16; ++k) toks[s][k] = slot.toks[k] < 2048 ? slot.toks[k] : 0;
      sim[s] = static_cast<int>(std::clamp<int64_t>(best >> 7, 0, 127));
      clauseText[s] = slot.text.substr(0, 93);
    }
  }

  std::vector<int> y1(128, 0), y2(128, 0);
  int prevTok = 1;
  int cursor[PE_STEPS] = {};
  bool used[8] = {};
  for (size_t t = 0; t < PE_TOKENS; ++t) {
    const size_t s = std::min<size_t>(2, t / 12);
    std::vector<int> u(128), y(128), z(128), h(128);
    for (size_t d = 0; d < 128; ++d) {
      u[d] = refSat8((m.tok[prevTok * 128 + d] + ((plan[s][d] + r[s][d]) >> 1) + m.pos[t * 128 + d]) >> 1);
    }
    const auto g = refLin(m.gate, u);
    for (size_t d = 0; d < 128; ++d) y[d] = static_cast<int>((u[d] * std::clamp<int64_t>(g[d] + 64, 0, 128) + 64) >> 7);
    for (size_t d = 0; d < 128; ++d) {
      const int64_t acc = m.dw.w[d * 3] * y2[d] + m.dw.w[d * 3 + 1] * y1[d] + m.dw.w[d * 3 + 2] * y[d];
      z[d] = refSat8(refSh(acc, m.dw.s) + m.dw.bias[d]);
    }
    y2 = y1;
    y1 = y;
    const auto o = refLin(m.outp, z);
    for (size_t d = 0; d < 128; ++d) h[d] = refSat8(u[d] + o[d]);
    auto logit = refLin(m.lm, h);
    const int64_t gate = std::clamp<int64_t>(refLin(m.copy, h)[0] + 64, 0, 128);
    const int tc = cursor[s] < 16 ? toks[s][cursor[s]] : 0;
    if (tc > 0) logit[tc] += (gate * sim[s]) >> 4;
    for (size_t v = 8; v < PE_VOCAB; ++v) {
      for (size_t j = t >= 6 ? t - 6 : 0; j < t; ++j) {
        if (e.tokens[j] == v) {
          logit[v] -= 10000;
          break;
        }
      }
    }
    int best = -1;
    for (size_t v = 0; v < PE_VOCAB; ++v) {
      if (v == 1) continue;
      if (v >= 2 && v <= 7 && used[v]) continue;
      if (v == 2 && subject.empty()) continue;
      if (v == 4 && echo.empty()) continue;
      if (v >= 5 && v <= 7 && e.clauseSlot[v - 5] < 0) continue;
      if (best < 0 || logit[v] > logit[best]) best = static_cast<int>(v);
    }
    if (best == 0) break;
    e.tokens[t] = static_cast<uint16_t>(best);
    prevTok = best;
    if (best >= 2 && best <= 7) used[best] = true;
    if (best >= 5 && best <= 7) {
      cursor[best - 5] = 16;
    } else if (tc > 0 && best == tc) {
      ++cursor[s];
    }
  }

  for (const uint16_t token : e.tokens) {
    if (token == 0) break;
    std::string piece = token >= 8    ? m.vocab[token].substr(0, 16)
                        : token == 2  ? subject
                        : token == 3  ? (role.empty() ? "this concept" : role)
                        : token == 4  ? echo
                                      : clauseText[token - 5];
    if (piece.empty()) continue;
    bool glue = piece.rfind("##", 0) == 0;
    if (glue) {
      piece.erase(0, 2);
    } else if (e.text.empty() || std::string(",.;:!?)'").find(piece[0]) != std::string::npos ||
               e.text.back() == '(') {
      glue = true;
    } else {
      size_t i = e.text.size() - 1;
      for (int k = 0; i > 0 && k < 3 && (static_cast<uint8_t>(e.text[i]) & 0xC0) == 0x80; ++k) --i;
      glue = refCjk(e.text, i, e.text.size()) && refCjk(piece, 0, 0);
    }
    if (!glue) e.text += ' ';
    e.text += piece;
  }
  if (e.text.size() > 239) {
    size_t cut = 239;
    while (cut > 0 && (static_cast<uint8_t>(e.text[cut]) & 0xC0) == 0x80) --cut;
    e.text.resize(cut);
  }
  return e;
}

struct PeRun {
  bool ok = false;
  std::string riff;
  int8_t plan[PE_STEPS][128] = {};
  uint8_t buckets[PE_STEPS] = {};
  uint16_t tokens[PE_TOKENS] = {};
};

PeRun runRiff(const std::string& dir, const int8_t* cPage, const int8_t* cEcho, const char* subject,
              const char* role, const char* echo) {
  PeRun run;
  memset(run.tokens, 0xFF, sizeof(run.tokens));
  run.ok = ce::generateMarginRiff(dir, cPage, cEcho, subject, role, echo, run.riff, run.plan, run.buckets, run.tokens);
  return run;
}

void expectMatchesReference(const PeRun& run, const PeExpect& want, const std::string& label) {
  EXPECT_EQ(memcmp(run.plan, want.plan, sizeof(want.plan)), 0) << label;
  for (size_t s = 0; s < PE_STEPS; ++s) EXPECT_EQ(run.buckets[s], want.buckets[s]) << label << " step " << s;
  for (size_t t = 0; t < PE_TOKENS; ++t) EXPECT_EQ(run.tokens[t], want.tokens[t]) << label << " token " << t;
  EXPECT_EQ(run.riff, want.text) << label;
  EXPECT_EQ(run.ok, !want.text.empty()) << label;
}

// A model whose logits are only its lm_head biases (zero weights, no copy
// boost), so the decode order is fixed by the biases, the masks and the
// repetition penalty.
PeModel biasOnlyModel(const uint32_t seed) {
  PeModel m = makePeModel(seed);
  std::fill(m.lm.w.begin(), m.lm.w.end(), 0);
  m.lm.s = 0;
  std::fill(m.lm.bias.begin(), m.lm.bias.end(), -1000);
  std::fill(m.copy.w.begin(), m.copy.w.end(), 0);
  m.copy.bias[0] = -64;  // gate_q7 = 0
  return m;
}

}  // namespace

TEST_F(ConceptEngineTest, PeWriterMatchesLiteralReferenceBitExact) {
  const PeModel model = makePeModel(777);
  const ClauseBank bank = makeClauseBank(4242);
  writeFile(dir / "pe-writer.bin", serializePe(model));
  writeFile(dir / "writer-clauses.bin", serializeClauses(bank));
  Lcg rng{99};
  // The random weights must exercise long decodes, several buckets and the
  // copy boost, not just an immediate EOS.
  size_t longRiffs = 0;
  std::vector<uint8_t> buckets;
  for (int c = 0; c < 24; ++c) {
    const auto page = randomBytes(rng, 128, 127);
    const auto echo = randomBytes(rng, 128, 127);
    const bool withEcho = c % 3 != 0;
    const char* subject = c % 4 == 1 ? "" : "Winston";
    const char* role = c % 2 ? "the last man" : "";
    const char* title = withEcho ? "Nineteen Eighty-Four" : "";
    const PeRun run = runRiff(dir.string(), page.data(), withEcho ? echo.data() : nullptr, subject, role, title);
    const PeExpect want =
        referenceRiff(model, bank, page.data(), withEcho ? echo.data() : nullptr, subject, role, title);
    expectMatchesReference(run, want, "case " + std::to_string(c));
    if (want.tokens[12] != 0) ++longRiffs;
    buckets.insert(buckets.end(), want.buckets, want.buckets + PE_STEPS);
  }
  std::sort(buckets.begin(), buckets.end());
  EXPECT_GE(longRiffs, 12u);
  EXPECT_GE(std::unique(buckets.begin(), buckets.end()) - buckets.begin(), 8);
}

TEST_F(ConceptEngineTest, PeWriterSyntheticRoundTrip) {
  PeModel model = biasOnlyModel(31);
  // Pointers first (each at most once), then two words; "reveals" and ","
  // are then inside the repetition window, so EOS wins.
  const int16_t order[] = {900, 800, 700, 600, 500, 400};
  for (size_t v = 2; v <= 7; ++v) model.lm.bias[v] = order[v - 2];
  model.lm.bias[8] = 300;
  model.lm.bias[9] = 290;
  model.lm.bias[0] = 100;
  ClauseBank bank = makeClauseBank(5);
  for (size_t j = 0; j < PE_SLOTS; ++j) bank[5 * PE_SLOTS + j].text = "bucket five clause";
  writeFile(dir / "pe-writer.bin", serializePe(model));
  writeFile(dir / "writer-clauses.bin", serializeClauses(bank));

  Lcg rng{7};
  const auto page = randomBytes(rng, 128, 127);
  const auto echo = randomBytes(rng, 128, 127);
  const PeRun run = runRiff(dir.string(), page.data(), echo.data(), "Winston", "the last man", "Nineteen Eighty-Four");
  ASSERT_TRUE(run.ok);

  // Thought vectors: unit norm at scale 127 and routed by their own argmax.
  for (size_t s = 0; s < PE_STEPS; ++s) {
    int64_t n2 = 0;
    for (const int8_t v : run.plan[s]) n2 += v * v;
    EXPECT_NEAR(std::sqrt(static_cast<double>(n2)), 127.0, 6.0) << "step " << s;
    int64_t best = INT64_MIN;
    size_t bestBucket = 0;
    for (size_t b = 0; b < PE_BUCKETS; ++b) {
      int64_t score = 0;
      for (size_t d = 0; d < 128; ++d) score += run.plan[s][d] * model.centroids[b * 128 + d];
      if (score > best) {
        best = score;
        bestBucket = b;
      }
    }
    EXPECT_EQ(run.buckets[s], bestBucket) << "step " << s;
  }
  EXPECT_NE(memcmp(run.plan[0], run.plan[1], 128), 0);

  const uint16_t expectTokens[PE_TOKENS] = {2, 3, 4, 5, 6, 7, 8, 9};
  for (size_t t = 0; t < PE_TOKENS; ++t) EXPECT_EQ(run.tokens[t], expectTokens[t]) << "token " << t;

  // Each <CLAUSE_s> expands to the best valid slot of the bucket step s routed to.
  std::string clauses[PE_STEPS];
  for (size_t s = 0; s < PE_STEPS; ++s) {
    int64_t best = INT64_MIN;
    for (size_t j = 0; j < PE_SLOTS; ++j) {
      const ClauseSlot& slot = bank[run.buckets[s] * PE_SLOTS + j];
      if (slot.text.empty()) continue;
      int64_t score = 0;
      for (size_t d = 0; d < 128; ++d) score += run.plan[s][d] * slot.vec[d];
      if (score > best) {
        best = score;
        clauses[s] = slot.text;
      }
    }
    ASSERT_FALSE(clauses[s].empty());
  }
  EXPECT_EQ(run.riff, "Winston the last man Nineteen Eighty-Four " + clauses[0] + " " + clauses[1] + " " + clauses[2] +
                          " reveals,");
  const PeExpect want = referenceRiff(model, bank, page.data(), echo.data(), "Winston", "the last man",
                                      "Nineteen Eighty-Four");
  expectMatchesReference(run, want, "round trip");

  // No role: "this concept"; no echo title or subject: their pointers are never chosen.
  const PeRun bare = runRiff(dir.string(), page.data(), echo.data(), "", "", "");
  const uint16_t bareTokens[PE_TOKENS] = {3, 5, 6, 7, 8, 9};
  for (size_t t = 0; t < PE_TOKENS; ++t) EXPECT_EQ(bare.tokens[t], bareTokens[t]) << "token " << t;
  EXPECT_EQ(bare.riff, "this concept " + clauses[0] + " " + clauses[1] + " " + clauses[2] + " reveals,");

  // A routed bucket without valid slots: no clause to point at.
  for (ClauseSlot& slot : bank) slot.text.clear();
  writeFile(dir / "writer-clauses.bin", serializeClauses(bank));
  const PeRun empty = runRiff(dir.string(), page.data(), echo.data(), "Winston", "", "Nineteen Eighty-Four");
  const uint16_t emptyTokens[PE_TOKENS] = {2, 3, 4, 8, 9};
  for (size_t t = 0; t < PE_TOKENS; ++t) EXPECT_EQ(empty.tokens[t], emptyTokens[t]) << "token " << t;
  EXPECT_EQ(empty.riff, "Winston this concept Nineteen Eighty-Four reveals,");
}

TEST_F(ConceptEngineTest, PeWriterNeverRepeatsAWordWithinSixSteps) {
  PeModel model = biasOnlyModel(32);
  for (size_t v = 2; v <= 7; ++v) model.lm.bias[v] = 2000;  // each pointer once, then never again
  for (size_t v = 8; v < 15; ++v) model.lm.bias[v] = static_cast<int16_t>(500 - v);
  model.lm.bias[0] = -9000;  // EOS loses to every unpenalized word
  writeFile(dir / "pe-writer.bin", serializePe(model));
  writeFile(dir / "writer-clauses.bin", serializeClauses(makeClauseBank(6)));
  Lcg rng{8};
  const auto page = randomBytes(rng, 128, 127);
  const PeRun run = runRiff(dir.string(), page.data(), nullptr, "Julia", "rebel", "Brave New World");
  ASSERT_TRUE(run.ok);
  int pointerCount[8] = {};
  for (size_t t = 0; t < PE_TOKENS; ++t) {
    const uint16_t v = run.tokens[t];
    ASSERT_NE(v, 0) << "token " << t;
    ASSERT_NE(v, 1) << "token " << t;
    if (v < 8) {
      ++pointerCount[v];
      continue;
    }
    for (size_t j = t >= 6 ? t - 6 : 0; j < t; ++j) EXPECT_NE(run.tokens[j], v) << "token " << t;
  }
  for (size_t v = 2; v <= 7; ++v) EXPECT_LE(pointerCount[v], 1) << "pointer " << v;
  for (size_t v = 2; v <= 4; ++v) EXPECT_EQ(pointerCount[v], 1) << "pointer " << v;
  // Seven words cycle with period seven once the pointers are spent.
  for (size_t t = 6; t + 7 < PE_TOKENS; ++t) EXPECT_EQ(run.tokens[t], run.tokens[t + 7]) << "token " << t;
  EXPECT_LE(run.riff.size(), ce::RIFF_MAX_BYTES);
  const PeExpect want = referenceRiff(model, makeClauseBank(6), page.data(), nullptr, "Julia", "rebel",
                                      "Brave New World");
  expectMatchesReference(run, want, "cycle");
}

TEST_F(ConceptEngineTest, PeWriterRendersGlueRulesAndTruncatesOnACodepoint) {
  PeModel model = biasOnlyModel(34);
  // ##ly ( beneath-the-tide 。 暗流 's counterbalances, — then the same seven
  // again (one is always outside the six-token window) up to 36 tokens.
  model.vocab[12] = "beneath-the-tide";  // 16 bytes, no NUL
  const uint16_t ids[] = {10, 11, 12, 13, 14, 15, 16};
  for (size_t i = 0; i < 7; ++i) model.lm.bias[ids[i]] = static_cast<int16_t>(900 - 10 * i);
  model.lm.bias[0] = 100;
  writeFile(dir / "pe-writer.bin", serializePe(model));
  writeFile(dir / "writer-clauses.bin", serializeClauses(makeClauseBank(11)));
  Lcg rng{12};
  const auto page = randomBytes(rng, 128, 127);
  const PeRun run = runRiff(dir.string(), page.data(), nullptr, "", "", "");
  ASSERT_TRUE(run.ok);
  for (size_t t = 0; t < PE_TOKENS; ++t) EXPECT_EQ(run.tokens[t], ids[t % 7]) << "token " << t;
  const std::string cycle = "ly (beneath-the-tide \xE3\x80\x82\xE6\x9A\x97\xE6\xB5\x81's counterbalances,";
  EXPECT_EQ(run.riff.rfind(cycle + "ly (beneath-the-tide", 0), 0u) << run.riff;
  EXPECT_LE(run.riff.size(), ce::RIFF_MAX_BYTES);
  EXPECT_GT(run.riff.size(), ce::RIFF_MAX_BYTES - 4);
  size_t at = 0;  // whole codepoints only
  while (at < run.riff.size()) {
    const auto lead = static_cast<uint8_t>(run.riff[at]);
    ASSERT_NE(lead & 0xC0, 0x80) << "byte " << at;
    at += lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
  }
  EXPECT_EQ(at, run.riff.size());
  expectMatchesReference(run, referenceRiff(model, makeClauseBank(11), page.data(), nullptr, "", "", ""), "glue");
}

TEST_F(ConceptEngineTest, PeWriterCopyBoostFollowsTheClauseCursor) {
  PeModel model = biasOnlyModel(33);
  for (size_t v = 8; v < PE_VOCAB; ++v) model.lm.bias[v] = 0;
  model.lm.bias[0] = -5;
  model.copy.bias[0] = 64;  // gate_q7 = 128
  ClauseBank bank = makeClauseBank(9);
  for (size_t i = 0; i < bank.size(); ++i) {
    bank[i].toks = {20, 21, 22, 23, 0};
    bank[i].text = "clause " + std::to_string(i);
  }
  writeFile(dir / "pe-writer.bin", serializePe(model));
  writeFile(dir / "writer-clauses.bin", serializeClauses(bank));
  Lcg rng{10};
  const auto page = randomBytes(rng, 128, 127);
  // Every slot vector becomes the step-0 thought (the planner does not read
  // the clauses), so sim_q7 is about 126 and the boost about 1008.
  const PeRun probe = runRiff(dir.string(), page.data(), nullptr, "", "", "");
  for (ClauseSlot& slot : bank) memcpy(slot.vec.data(), probe.plan[0], 128);
  writeFile(dir / "writer-clauses.bin", serializeClauses(bank));
  const PeRun run = runRiff(dir.string(), page.data(), nullptr, "", "", "");
  // The boosted clause tokens come out in clause order; then the tied plain
  // words (bias 0) from the lowest unpenalized id.
  EXPECT_EQ(run.tokens[0], 20);
  EXPECT_EQ(run.tokens[1], 21);
  EXPECT_EQ(run.tokens[2], 22);
  EXPECT_EQ(run.tokens[3], 23);
  const PeExpect want = referenceRiff(model, bank, page.data(), nullptr, "", "", "");
  expectMatchesReference(run, want, "copy");
}

TEST_F(ConceptEngineTest, PeWriterRejectsMalformedFiles) {
  PeModel model = biasOnlyModel(1);
  model.lm.bias[8] = 300;  // "reveals", then EOS
  const auto pe = serializePe(model);
  const auto clauses = serializeClauses(makeClauseBank(2));
  int8_t page[128] = {};
  page[0] = 127;
  std::string riff = "stale";
  // Missing files.
  EXPECT_FALSE(ce::generateMarginRiff(dir.string(), page, nullptr, "a", "b", "c", riff));
  EXPECT_TRUE(riff.empty());
  writeFile(dir / "writer-clauses.bin", clauses);
  EXPECT_FALSE(ce::generateMarginRiff(dir.string(), page, nullptr, "a", "b", "c", riff));
  writeFile(dir / "pe-writer.bin", pe);
  EXPECT_TRUE(ce::generateMarginRiff(dir.string() + "/", page, nullptr, "a", "b", "c", riff));
  EXPECT_EQ(riff, "reveals");

  auto bad = pe;
  bad[0] = 'X';
  writeFile(dir / "pe-writer.bin", bad);
  EXPECT_FALSE(ce::generateMarginRiff(dir.string(), page, nullptr, "a", "b", "c", riff));
  bad = pe;
  bad[13] = 0;  // n_vocab
  writeFile(dir / "pe-writer.bin", bad);
  EXPECT_FALSE(ce::generateMarginRiff(dir.string(), page, nullptr, "a", "b", "c", riff));
  bad = pe;
  const float notPow2 = 0.3f;
  memcpy(bad.data() + 342580, &notPow2, 4);  // lm_head scale
  writeFile(dir / "pe-writer.bin", bad);
  EXPECT_FALSE(ce::generateMarginRiff(dir.string(), page, nullptr, "a", "b", "c", riff));
  bad = pe;
  bad.pop_back();
  writeFile(dir / "pe-writer.bin", bad);
  EXPECT_FALSE(ce::generateMarginRiff(dir.string(), page, nullptr, "a", "b", "c", riff));

  writeFile(dir / "pe-writer.bin", pe);
  auto badClauses = clauses;
  badClauses[10] = 128;  // slot_bytes
  writeFile(dir / "writer-clauses.bin", badClauses);
  EXPECT_FALSE(ce::generateMarginRiff(dir.string(), page, nullptr, "a", "b", "c", riff));
  badClauses = clauses;
  badClauses.resize(badClauses.size() - 256);
  writeFile(dir / "writer-clauses.bin", badClauses);
  EXPECT_FALSE(ce::generateMarginRiff(dir.string(), page, nullptr, "a", "b", "c", riff));
  EXPECT_TRUE(riff.empty());
}

TEST_F(ConceptEngineTest, MarginNotesKeepTheNewestSixtyFour) {
  const std::string path = (dir / "margin-notes.txt").string();
  for (int i = 0; i < 70; ++i) {
    ASSERT_TRUE(ce::appendMarginNote("Book p." + std::to_string(i + 1) + " — word: riff\nline " + std::to_string(i),
                                     path.c_str()));
  }
  EXPECT_FALSE(ce::appendMarginNote("", path.c_str()));
  std::ifstream in(path);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) lines.push_back(line);
  ASSERT_EQ(lines.size(), ce::MARGIN_NOTES_MAX);
  EXPECT_EQ(lines.front(), "Book p.7 — word: riff line 6");
  EXPECT_EQ(lines.back(), "Book p.70 — word: riff line 69");
  EXPECT_FALSE(fs::exists(path + ".tmp"));
}

TEST_F(ConceptEngineTest, QueryMatchesCarryTheirVectors) {
  int8_t vec[128];
  ce::encodeText("the whale breached beside the boat", vec);
  ASSERT_TRUE(ce::appendEpisodicVector("/books/Moby Dick.epub", 4, vec));
  const auto matches = ce::queryEpisodicAndShelf(vec, 1, false);
  ASSERT_EQ(matches.size(), 1u);
  EXPECT_EQ(memcmp(matches[0].vec, vec, 128), 0);
}

// Trainer golden cases: CONCEPT_ENGINE_GOLDEN_DIR=<dir with pe-writer.bin,
// writer-clauses.bin, pe_writer_golden.bin> (or its pe_writer/ subdirectory).
TEST_F(ConceptEngineTest, PeWriterMatchesGoldenVectors) {
  const char* goldenDir = std::getenv("CONCEPT_ENGINE_GOLDEN_DIR");
  if (!goldenDir || !*goldenDir) GTEST_SKIP() << "CONCEPT_ENGINE_GOLDEN_DIR not set";
  fs::path golden(goldenDir);
  if (!fs::exists(golden / "pe_writer_golden.bin")) golden /= "pe_writer";
  if (!fs::exists(golden / "pe_writer_golden.bin")) GTEST_SKIP() << "no pe_writer_golden.bin";
  std::ifstream in(golden / "pe_writer_golden.bin", std::ios::binary);
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  ASSERT_GE(bytes.size(), 8u);
  ASSERT_EQ(memcmp(bytes.data(), "PWGD", 4), 0);
  uint32_t cases;
  memcpy(&cases, bytes.data() + 4, 4);
  constexpr size_t RECORD_BYTES = 128 + 128 + 32 * 3 + 3 * 128 + 3 + 36 * 2 + 240;
  static_assert(RECORD_BYTES == 1051);
  ASSERT_EQ(bytes.size(), 8 + cases * RECORD_BYTES);
  EXPECT_EQ(cases, 32u);
  const auto field = [](const uint8_t* p, const size_t n) {
    const auto* c = reinterpret_cast<const char*>(p);
    return std::string(c, strnlen(c, n));
  };
  size_t exact = 0;
  for (uint32_t c = 0; c < cases; ++c) {
    const uint8_t* p = bytes.data() + 8 + c * RECORD_BYTES;
    const auto* page = reinterpret_cast<const int8_t*>(p);
    const auto* echo = reinterpret_cast<const int8_t*>(p + 128);
    const std::string subject = field(p + 256, 32);
    const std::string role = field(p + 288, 32);
    const std::string title = field(p + 320, 32);
    const uint8_t* expected = p + 352;
    uint16_t tokens[PE_TOKENS];
    memcpy(tokens, expected + 384 + 3, sizeof(tokens));
    const std::string text = field(expected + 384 + 3 + 72, 240);
    const PeRun run = runRiff(golden.string(), page, echo, subject.c_str(), role.c_str(), title.c_str());
    const bool planOk = memcmp(run.plan, expected, 384) == 0;
    const bool bucketsOk = memcmp(run.buckets, expected + 384, 3) == 0;
    const bool tokensOk = memcmp(run.tokens, tokens, sizeof(tokens)) == 0;
    EXPECT_TRUE(planOk) << "case " << c;
    EXPECT_TRUE(bucketsOk) << "case " << c;
    EXPECT_TRUE(tokensOk) << "case " << c;
    EXPECT_EQ(run.riff, text) << "case " << c;
    EXPECT_EQ(run.ok, !text.empty()) << "case " << c;
    if (planOk && bucketsOk && tokensOk && run.riff == text) ++exact;
  }
  EXPECT_EQ(exact, cases);
}

// ---- Lookup sections: excerpts, Related notes, story timeline, extended riff ----------------

namespace {

std::string timeline(const std::string& word, const char* book, const uint32_t cutoff, const size_t cap = 1536) {
  std::vector<char> out(cap, '\x7f');
  if (!ce::weaveExtendedRefresher(word.data(), word.size(), book, cutoff, out.data(), out.size())) return "<none>";
  return out.data();
}

// The concept vector the graph file stores for the event whose gloss is `gloss`.
std::array<int8_t, 128> eventVec(const std::vector<uint8_t>& graph, const std::string& gloss) {
  std::array<int8_t, 128> vec{};
  const auto at = std::search(graph.begin(), graph.end(), gloss.begin(), gloss.end());
  EXPECT_NE(at, graph.end()) << gloss;
  if (at == graph.end()) return vec;
  const size_t event = static_cast<size_t>(at - graph.begin()) - 8;
  memcpy(vec.data(), graph.data() + event + 128, 128);
  return vec;
}

std::string words(const int count) {
  std::string text;
  for (int i = 0; i < count; ++i) {
    char word[8];
    snprintf(word, sizeof(word), "w%02d", i);
    if (!text.empty()) text += (i % 7 == 0 ? "\n\t " : " ");
    text += word;
  }
  return text;
}

}  // namespace

TEST(ConceptEngineExcerpt, CollapsesSpaceAndCentersOnWholeWords) {
  EXPECT_EQ(ce::excerptAround("  short\n\ttext  here "), "short text here");
  EXPECT_EQ(ce::excerptAround(""), "");
  const std::string excerpt = ce::excerptAround(words(60));
  ASSERT_LE(excerpt.size(), ce::EXCERPT_BYTES);
  EXPECT_GT(excerpt.size(), ce::EXCERPT_BYTES / 2);
  EXPECT_EQ(excerpt.rfind("\xE2\x80\xA6w", 0), 0u) << excerpt;
  EXPECT_EQ(excerpt.compare(excerpt.size() - 3, 3, "\xE2\x80\xA6"), 0) << excerpt;
  EXPECT_NE(excerpt.find("w30"), std::string::npos) << excerpt;
  EXPECT_EQ(excerpt.find("w00"), std::string::npos) << excerpt;
  EXPECT_EQ(excerpt.find("  "), std::string::npos);
  EXPECT_EQ(excerpt.find('\n'), std::string::npos);
  // Each kept word is whole: the text between the marks splits into wNN words.
  std::istringstream kept(excerpt.substr(3, excerpt.size() - 6));
  std::string word;
  while (kept >> word) EXPECT_EQ(word.size(), 3u) << word;
  // CJK text is cut on code points.
  std::string cjk;
  for (int i = 0; i < 80; ++i) cjk += "斗";
  const std::string cut = ce::excerptAround(cjk, 40);
  EXPECT_LE(cut.size(), 40u);
  EXPECT_EQ((cut.size() - 6) % 3, 0u);
}

TEST_F(ConceptEngineTest, EpisodicExcerptSidecarRoundTrips) {
  const std::string whale = "Call me Ishmael. " + words(60) + " the whale breached beside the harpoon boat";
  ASSERT_TRUE(ce::appendEpisodicPage("/books/Moby Dick.epub", ce::epubPosition(3, 11), whale));
  ASSERT_TRUE(ce::appendEpisodicPage("/books/Cookbook.txt", 41, "whisk the eggs with sugar and butter"));
  int8_t vec[128];
  ce::encodeText("electrons orbit the nucleus", vec);
  ASSERT_TRUE(ce::appendEpisodicVector("/books/Physics.epub", 7, vec));
  // The .idx layout is unchanged; excerpts live in the sidecar.
  EXPECT_EQ(fs::file_size(episodicPath), 16u + 3 * 192);
  const fs::path sidecar = dir / "episodic.exc";
  ASSERT_TRUE(fs::exists(sidecar));
  EXPECT_EQ(fs::file_size(sidecar) % 176, 0u);

  const auto cook = ce::queryEpisodicAndShelf("whisk eggs and sugar", 1);
  ASSERT_EQ(cook.size(), 1u);
  EXPECT_EQ(cook[0].excerpt, "whisk the eggs with sugar and butter");
  EXPECT_EQ(cook[0].index, 1u);
  const auto moby = ce::queryEpisodicAndShelf(whale, 1);
  ASSERT_EQ(moby.size(), 1u);
  EXPECT_EQ(moby[0].excerpt, ce::excerptAround(whale));
  EXPECT_EQ(moby[0].index, 0u);
  const auto physics = ce::queryEpisodicAndShelf(vec, 1, false);
  ASSERT_EQ(physics.size(), 1u);
  EXPECT_EQ(physics[0].excerpt, "");

  // Revisiting a page replaces its excerpt with the new text.
  ASSERT_TRUE(ce::appendEpisodicPage("/books/Cookbook.txt", 41, "fold the egg whites into the batter"));
  EXPECT_EQ(fs::file_size(episodicPath), 16u + 3 * 192);
  const auto folded = ce::queryEpisodicAndShelf("fold the egg whites into the batter", 1);
  ASSERT_EQ(folded.size(), 1u);
  EXPECT_EQ(folded[0].excerpt, "fold the egg whites into the batter");

  // A sidecar slot whose book or page disagrees with the record is ignored.
  fs::resize_file(sidecar, 0);
  std::ofstream(sidecar, std::ios::binary) << std::string(176 * 3, '\x55');
  const auto stale = ce::queryEpisodicAndShelf("fold the egg whites into the batter", 1);
  ASSERT_EQ(stale.size(), 1u);
  EXPECT_EQ(stale[0].excerpt, "");
}

TEST(ConceptEngineRelated, CompactLinesAndExtendedItems) {
  ce::Match moby;
  moby.label = "Moby Dick";
  moby.page = ce::epubPosition(3, 11);
  moby.excerpt = ce::excerptAround(words(60));
  ce::Match essay;
  essay.label = "Essay";
  essay.shelf = true;
  ce::Match scene;
  scene.label = "Wu Tan City";
  scene.story = true;
  scene.page = 3;
  scene.excerpt = "Xiao Clan's home city (Ch.3)";
  ce::Match echo;
  echo.label = "Martial Peak: fallen genius returns";
  echo.story = true;
  echo.crossBook = true;
  echo.excerpt = "rise-from-ruin arc";
  ce::Match fifth = moby;
  fifth.label = "Fifth";
  const std::vector<ce::Match> matches = {moby, essay, scene, echo, fifth};

  EXPECT_EQ(ce::formatRelatedNotes({}, false), "");
  EXPECT_EQ(ce::formatRelatedNotes({}, true), "");

  const std::string compact = ce::formatRelatedNotes(matches, false);
  const size_t newline = compact.find('\n');
  ASSERT_NE(newline, std::string::npos);
  const std::string first = compact.substr(0, newline);
  EXPECT_EQ(compact.substr(newline + 1), "Essay (Inbox)");
  EXPECT_EQ(first.rfind("Moby Dick ch.4 p.12 \xE2\x80\x94 \xE2\x80\x9C\xE2\x80\xA6", 0), 0u) << first;
  EXPECT_LE(first.size(), ce::RELATED_COMPACT_LINE_BYTES);
  EXPECT_EQ(first.compare(first.size() - 6, 6, "\xE2\x80\xA6\xE2\x80\x9D"), 0) << first;
  EXPECT_LT(first.size(), moby.excerpt.size() + 30);

  const std::string extended = ce::formatRelatedNotes(matches, true);
  EXPECT_EQ(extended, "\xE2\x80\xA2 Moby Dick ch.4 p.12\n\xE2\x80\x9C" + moby.excerpt +
                          "\xE2\x80\x9D\n\n\xE2\x80\xA2 Essay (Inbox)\n\n\xE2\x80\xA2 Wu Tan City Ch.3\n\xE2\x80\x9C"
                          "Xiao Clan's home city (Ch.3)\xE2\x80\x9D\n\n\xE2\x80\xA2 Echo: Martial Peak: fallen genius "
                          "returns\n\xE2\x80\x9Crise-from-ruin arc\xE2\x80\x9D");
  EXPECT_EQ(extended.find("Fifth"), std::string::npos);
  EXPECT_GT(extended.size(), compact.size());

  // Items that would pass the byte cap are left out whole.
  std::vector<ce::Match> big(4, moby);
  for (auto& m : big) m.excerpt = std::string(400, 'x');
  const std::string capped = ce::formatRelatedNotes(big, true);
  EXPECT_LE(capped.size(), ce::RELATED_EXTENDED_BYTES);
  EXPECT_EQ(std::count(capped.begin(), capped.end(), '\n'), 2 + 2);  // two items: one excerpt line each, one gap
}

TEST_F(ConceptEngineTest, StoryTimelineListsRevealedScenesInOrder) {
  writeFile(storyPath, makeDoupoGraph());
  const std::string ch4 = timeline("萧炎", DOUPO, chapter(4));
  EXPECT_EQ(ch4,
            "Xiao Yan (Dou Qi disciple)"
            "\n\xE2\x80\xA2 Ch.1: [origin] introduced at Xiao Clan test (Ch.1)"
            "\n\xE2\x80\xA2 Ch.2: [role] shamed at the Dou Qi test (Ch.2) -> Xiao Clan"
            "\n\xE2\x80\xA2 Ch.3: [arc] clashed over 3-year pact (Ch.3) -> Wu Tan City");
  // The compact refresher is a subset; echoes stay out of the timeline.
  EXPECT_GT(ch4.size(), weave("萧炎", DOUPO, chapter(4)).size());
  EXPECT_EQ(timeline("萧炎", DOUPO, chapter(6)), ch4);
  EXPECT_EQ(ch4.find("Flame Emperor"), std::string::npos);
  EXPECT_NE(timeline("萧炎", DOUPO, chapter(500)).find("Flame Emperor"), std::string::npos);
  EXPECT_EQ(timeline("萧炎", DOUPO, chapter(1)),
            "Xiao Yan (Dou Qi disciple)\n\xE2\x80\xA2 Ch.1: [origin] introduced at Xiao Clan test (Ch.1)");
  // Unrevealed aliases and entities give nothing.
  EXPECT_EQ(timeline("炎帝", DOUPO, chapter(6)), "<none>");
  EXPECT_EQ(timeline("美杜莎", DOUPO, chapter(6)), "<none>");
  // A small cap drops whole scene lines.
  const std::string tight = timeline("萧炎", DOUPO, chapter(4), 120);
  EXPECT_EQ(tight, ch4.substr(0, tight.size()));
  EXPECT_EQ(tight.back(), ')');
}

TEST_F(ConceptEngineTest, StoryTimelineKeepsEndsAndMostSalientMiddles) {
  const char* book = "/books/Saga.epub";
  std::vector<SgEvent> events;
  const uint8_t salience[] = {10, 50, 200, 30, 180, 20, 150, 160, 5};
  for (uint16_t i = 0; i < 9; ++i) {
    events.push_back({static_cast<uint32_t>(100 + i), static_cast<uint16_t>(i + 1), 0, salience[i],
                      "scene " + std::to_string(i + 1), ""});
  }
  writeFile(storyPath, makeStoryGraph({{"hero", book, 0, "Hero", events}}, {}));
  EXPECT_EQ(timeline("hero", book, 1000),
            "Hero"
            "\n\xE2\x80\xA2 Ch.1: [role] scene 1"
            "\n\xE2\x80\xA2 Ch.3: [role] scene 3"
            "\n\xE2\x80\xA2 Ch.5: [role] scene 5"
            "\n\xE2\x80\xA2 Ch.7: [role] scene 7"
            "\n\xE2\x80\xA2 Ch.8: [role] scene 8"
            "\n\xE2\x80\xA2 Ch.9: [role] scene 9");
}

TEST_F(ConceptEngineTest, StoryEchoesAreSpoilerGatedAndBookScoped) {
  const auto graph = makeDoupoGraph();
  writeFile(storyPath, graph);
  const auto home = eventVec(graph, "Xiao Clan's home city (Ch.3)");
  const auto medusa = eventVec(graph, "Snake-People queen met in the desert (Ch.300)");

  // Wu Tan City is revealed at chapter 3; the focus entity's own echo comes
  // first once revealed (chapter 5), and never before.
  const auto ch4 = ce::queryStoryEchoes(home.data(), DOUPO, chapter(4), "萧炎");
  ASSERT_FALSE(ch4.empty());
  EXPECT_FALSE(ch4[0].crossBook);
  const auto homeHit = std::find_if(ch4.begin(), ch4.end(),
                                    [](const ce::Match& m) { return m.label == "Wu Tan City (Xiao Clan seat)"; });
  ASSERT_NE(homeHit, ch4.end());
  EXPECT_TRUE(homeHit->story);
  EXPECT_EQ(homeHit->page, 3u);
  EXPECT_EQ(homeHit->excerpt, "Xiao Clan's home city (Ch.3)");
  EXPECT_EQ(ce::describeMatch(*homeHit), "Wu Tan City (Xiao Clan seat) Ch.3");
  EXPECT_EQ(memcmp(homeHit->vec, home.data(), 128), 0);
  for (const auto& m : ch4) {
    EXPECT_GE(m.dot, ce::RELATED_MIN_DOT);
    EXPECT_EQ(m.pathHash, ce::fnv1a64(DOUPO));
    EXPECT_EQ(m.label.find("Xiao Yan"), std::string::npos) << "focus scenes belong to the Story tab";
    EXPECT_EQ(m.label.find("Medusa"), std::string::npos);
  }

  const auto ch6 = ce::queryStoryEchoes(home.data(), DOUPO, chapter(6), "萧炎");
  ASSERT_FALSE(ch6.empty());
  EXPECT_TRUE(ch6[0].crossBook);
  EXPECT_EQ(ch6[0].label, "Martial Peak: fallen genius returns");
  EXPECT_EQ(ch6[0].excerpt, "rise-from-ruin arc");
  EXPECT_EQ(ch6[0].dot, INT32_MAX);

  // Before chapter 3 the city is unrevealed; Medusa only from chapter 300.
  for (const auto& m : ce::queryStoryEchoes(home.data(), DOUPO, chapter(2), "")) {
    EXPECT_NE(m.label, "Wu Tan City (Xiao Clan seat)");
  }
  for (const auto& m : ce::queryStoryEchoes(medusa.data(), DOUPO, chapter(299), "")) {
    EXPECT_EQ(m.label.find("Medusa"), std::string::npos);
  }
  const auto later = ce::queryStoryEchoes(medusa.data(), DOUPO, chapter(300), "");
  ASSERT_FALSE(later.empty());
  EXPECT_EQ(later[0].label, "Medusa (Snake-People queen)");
  EXPECT_EQ(later[0].excerpt, "Snake-People queen met in the desert (Ch.300) \xE2\x80\x94 Tagoo Desert");

  // Another book's reader sees none of these scenes.
  EXPECT_TRUE(ce::queryStoryEchoes(home.data(), "/books/Other.txt", chapter(500), "萧炎").empty());
  EXPECT_TRUE(ce::queryStoryEchoes(home.data(), DOUPO, chapter(500), "", 0).empty());
}

TEST_F(ConceptEngineTest, ExtendedRiffWritesThreeSentencesOfUniqueClauses) {
  const PeModel model = makePeModel(777);
  ClauseBank bank = makeClauseBank(4242);
  for (size_t i = 0; i < bank.size(); ++i) {
    if (bank[i].text.empty()) continue;
    bank[i].text = "clause " + std::to_string(i) + " weighs loyalty against self-interest" + (i % 2 ? "." : " ");
  }
  writeFile(dir / "pe-writer.bin", serializePe(model));
  writeFile(dir / "writer-clauses.bin", serializeClauses(bank));
  Lcg rng{99};
  for (int c = 0; c < 6; ++c) {
    const auto page = randomBytes(rng, 128, 127);
    const auto echo = randomBytes(rng, 128, 127);
    const bool withEcho = c % 2 == 0;
    std::string compactWant;
    ce::generateMarginRiff(dir.string(), page.data(), withEcho ? echo.data() : nullptr, "Winston", "the last man",
                           withEcho ? "Nineteen Eighty-Four" : "", compactWant);
    std::string riff = "stale";
    std::string compact = "stale";
    uint16_t ids[ce::RIFF_EXTENDED_CLAUSES];
    ASSERT_TRUE(ce::generateExtendedMarginRiff(dir.string(), page.data(), withEcho ? echo.data() : nullptr, "Winston",
                                               "the last man", withEcho ? "Nineteen Eighty-Four" : "", riff, &compact,
                                               ids))
        << "case " << c;
    EXPECT_EQ(compact, compactWant) << "case " << c;
    EXPECT_GT(riff.size(), 240u) << riff;
    EXPECT_LE(riff.size(), ce::RIFF_EXTENDED_MAX_BYTES);
    EXPECT_EQ(riff.rfind("Winston, the last man: ", 0), 0u) << riff;
    EXPECT_NE(riff.find(withEcho ? ". Beside Nineteen Eighty-Four: " : ". In context: "), std::string::npos) << riff;
    EXPECT_NE(riff.find(". The tension: "), std::string::npos) << riff;
    EXPECT_EQ(riff.back(), '.');
    EXPECT_EQ(riff.find(".;"), std::string::npos) << riff;
    EXPECT_EQ(riff.find(" ;"), std::string::npos) << riff;
    EXPECT_EQ(riff.find(".."), std::string::npos) << riff;
    std::vector<uint16_t> used(ids, ids + ce::RIFF_EXTENDED_CLAUSES);
    for (const uint16_t id : used) {
      ASSERT_LT(id, bank.size()) << "case " << c;
      const std::string clause = "clause " + std::to_string(id) + " weighs";
      EXPECT_NE(riff.find(clause), std::string::npos) << clause;
    }
    std::sort(used.begin(), used.end());
    EXPECT_EQ(std::unique(used.begin(), used.end()), used.end()) << "case " << c;
  }

  // Without the model files there is no riff of either length.
  fs::remove(dir / "writer-clauses.bin");
  const auto page = randomBytes(rng, 128, 127);
  std::string riff = "stale";
  std::string compact = "stale";
  EXPECT_FALSE(ce::generateExtendedMarginRiff(dir.string(), page.data(), nullptr, "", "", "", riff, &compact));
  EXPECT_EQ(riff, "");
  EXPECT_EQ(compact, "");
}
