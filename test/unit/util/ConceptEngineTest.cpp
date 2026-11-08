#include <gtest/gtest.h>
#include <unistd.h>

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

std::array<int8_t, 128> refForward(const std::vector<uint8_t>& model, const Shifts& s, const std::string& text,
                                   const std::vector<uint8_t>* engram = nullptr, const int sg = 5) {
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
  std::vector<std::pair<size_t, size_t>> spans;
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
  std::string modelPath, engramPath, episodicPath, shelfPath;

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
    ce::Paths paths;
    paths.model = modelPath.c_str();
    paths.episodic = episodicPath.c_str();
    paths.shelf = shelfPath.c_str();
    ce::setPaths(paths);
    ce::unloadModel();
  }

  void TearDown() override {
    ce::unloadModel();
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

// The trainer's artifacts: CONCEPT_ENGINE_GOLDEN_DIR=<dir with platonic-hnet.bin,
// engram.bin, golden.jsonl ({"text","hex","vec",...} per line) and
// engram_gloss_examples.json ([{"word","gloss"}, ...])>.
const char* goldenDir() {
  const char* dir = std::getenv("CONCEPT_ENGINE_GOLDEN_DIR");
  return dir && *dir ? dir : nullptr;
}

// The JSON string value after `"key":` at or past `from`; handles \" and \\ only.
std::string jsonString(const std::string& text, const std::string& key, size_t from = 0) {
  const size_t at = text.find("\"" + key + "\"", from);
  if (at == std::string::npos) return {};
  size_t i = text.find('"', text.find(':', at)) + 1;
  std::string out;
  for (; i < text.size() && text[i] != '"'; ++i) {
    if (text[i] == '\\') ++i;
    out += text[i];
  }
  return out;
}

TEST_F(ConceptEngineTest, MatchesTrainerGoldenVectors) {
  if (!goldenDir()) GTEST_SKIP() << "CONCEPT_ENGINE_GOLDEN_DIR not set";
  const fs::path golden(goldenDir());
  const std::string model = (golden / "platonic-hnet.bin").string();
  const std::string engram = (golden / "engram.bin").string();
  ce::Paths paths;
  paths.model = model.c_str();
  paths.episodic = episodicPath.c_str();
  paths.shelf = shelfPath.c_str();
  ce::setPaths(paths);
  std::ifstream lines(golden / "golden.jsonl");
  ASSERT_TRUE(lines.good());
  std::string line;
  size_t checked = 0;
  while (std::getline(lines, line)) {
    // Exact input bytes come from "hex" (the text may be malformed UTF-8).
    const std::string hex = jsonString(line, "hex");
    std::string text;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) text += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
    std::array<int8_t, 128> want{};
    std::stringstream vec(line.substr(line.find('[', line.find("\"vec\"")) + 1));
    for (auto& v : want) {
      int value;
      char sep;
      vec >> value >> sep;
      v = static_cast<int8_t>(value);
    }
    std::array<int8_t, 128> got{};
    ASSERT_TRUE(ce::encodeText(text, got.data(), engram.c_str()));
    EXPECT_EQ(got, want) << "hex " << hex;
    ++checked;
  }
  EXPECT_GT(checked, 0u);
}

TEST_F(ConceptEngineTest, MatchesTrainerGlossExamples) {
  if (!goldenDir()) GTEST_SKIP() << "CONCEPT_ENGINE_GOLDEN_DIR not set";
  const std::string engram = (fs::path(goldenDir()) / "engram.bin").string();
  std::ifstream file(fs::path(goldenDir()) / "engram_gloss_examples.json");
  ASSERT_TRUE(file.good());
  const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  size_t checked = 0;
  for (size_t at = json.find("\"word\""); at != std::string::npos; at = json.find("\"word\"", at + 1)) {
    const std::string word = jsonString(json, "word", at);
    const std::string gloss = jsonString(json, "gloss", at);
    std::string matched;
    EXPECT_EQ(ce::lookupEngramGloss(word, &matched, 1, engram.c_str()), gloss) << word;
    EXPECT_EQ(matched, word);
    // Held mid-sentence: trailing text and punctuation still find the compound.
    EXPECT_EQ(ce::lookupEngramGloss("\xE2\x80\x9C" + word, nullptr, 1, engram.c_str()), gloss) << word;
    ++checked;
  }
  EXPECT_GT(checked, 0u);
}

// ---- ENGR gloss ------------------------------------------------------------------

std::string gloss(const std::string& text, const std::string& engram, std::string* matched = nullptr,
                  const size_t minCjkChars = 1) {
  return ce::lookupEngramGloss(text, matched, minCjkChars, engram.c_str());
}

TEST_F(ConceptEngineTest, EngramGlossNeedsExactSpanHash) {
  constexpr uint32_t slots = 64;
  const uint64_t rizz = ce::fnv1a64("rizz");
  writeFile(engramPath, makeEngram(slots, {{rizz, {}, "charisma, esp. in flirting"}}));
  std::string matched;
  EXPECT_EQ(gloss("rizz", engramPath, &matched), "charisma, esp. in flirting");
  EXPECT_EQ(matched, "rizz");
  EXPECT_EQ(gloss("\xE2\x80\x9CRizz,\xE2\x80\x9D", engramPath), "charisma, esp. in flirting");
  EXPECT_EQ(gloss("(RIZZ)...", engramPath), "charisma, esp. in flirting");
  // A word landing in the same slot with a different hash is not a match.
  std::string collider;
  for (int i = 0; collider.empty(); ++i) {
    const std::string candidate = "w" + std::to_string(i);
    if (ce::fnv1a64(candidate) % slots == rizz % slots) collider = candidate;
  }
  EXPECT_EQ(gloss(collider, engramPath), "");
  EXPECT_EQ(gloss("rizz", (dir / "absent.bin").string()), "");
  EXPECT_EQ(gloss("", engramPath), "");
  EXPECT_EQ(gloss("--", engramPath), "");
}

TEST_F(ConceptEngineTest, EngramGlossFoldsLatinCase) {
  writeFile(engramPath, makeEngram(64, {{ce::fnv1a64("\xC3\xBC" "ber"), {}, "over, beyond"},
                                        {ce::fnv1a64("\xD0\xB4\xD0\xB0"), {}, "yes"}}));
  EXPECT_EQ(gloss("\xC3\x9C" "ber", engramPath), "over, beyond");  // Über
  EXPECT_EQ(gloss("\xD0\x94\xD0\x90!", engramPath), "yes");         // ДА!
}

TEST_F(ConceptEngineTest, EngramGlossTriesLongestCjkCompound) {
  // 不知道 and 知道 both have glosses; 不 alone does not.
  writeFile(engramPath, makeEngram(4096, {{ce::fnv1a64("不知道"), {}, "no+to know"},
                                          {ce::fnv1a64("知道"), {}, "to know"},
                                          {ce::fnv1a64("ABC"), {}, "uppercase key"}}));
  std::string matched;
  EXPECT_EQ(gloss("不知道他在哪", engramPath, &matched), "no+to know");
  EXPECT_EQ(matched, "不知道");
  EXPECT_EQ(gloss("知道了。", engramPath, &matched), "to know");
  EXPECT_EQ(matched, "知道");
  // From one character earlier the compound must still cover the held one.
  EXPECT_EQ(gloss("不在", engramPath, nullptr, 2), "");
  EXPECT_EQ(gloss("不知", engramPath, nullptr, 2), "");
  EXPECT_EQ(gloss("不知道", engramPath, nullptr, 2), "no+to know");
  // Latin text is looked up lowercased only.
  EXPECT_EQ(gloss("ABC", engramPath), "");
}

TEST_F(ConceptEngineTest, EngramGlossRejectsBadHeader) {
  auto engram = makeEngram(64, {{ce::fnv1a64("yeet"), {}, "to throw"}});
  engram[12] = 32;  // slot_bytes
  writeFile(engramPath, engram);
  EXPECT_EQ(gloss("yeet", engramPath), "");
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
