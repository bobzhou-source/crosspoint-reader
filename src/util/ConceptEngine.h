#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// On-device concept vectors for the reader: dictionary sense ranking, Engram
// glosses for words no dictionary knows, and "Related" echoes from pages read
// before and from the Inbox shelf.
//
// Model: /models/platonic-hnet.bin (PLHN v1, 95,008 bytes), run with the
// integer-only forward of the PlatonicEngramHNet trainer's plhn_int_forward
// (bit-exact with the companion's encoder): byte embedding -> two gated
// ShortConv(k=3) blocks -> H-Net boundary router (dot(q_t, k_{t-1}) <= 0, spans
// of at most 12 bytes, never splitting a UTF-8 character) -> chunk mean ->
// chunk_proj -> Engram delta -> shared core block looped K=2 -> sum -> L2 to
// int8 at scale 127. The weights load lazily into PSRAM on first use. Without
// the model (or PSRAM) every entry point still works on a deterministic
// character-trigram SimHash projection, so the features need no SD model.
//
// Binary formats (all little-endian):
// - engram.bin: 512-byte header sector ("ENGR", version 1, num_slots,
//   slot_bytes = 64, float scale = 2^-Sg), then 64-byte slots: u64 FNV-1a 64
//   span hash (0 = empty), int8 concept_delta[24], char gloss[32].
// - episodic.idx / inbox-concepts.idx: 16-byte header ("ICPT", version 1,
//   count, dim = 128), then 192-byte records: u64 FNV-1a 64 of the path,
//   u32 page_or_chunk, u32 timestamp_or_flags, char label[48], int8 vec[128].
namespace ConceptEngine {

constexpr size_t DIM = 128;
constexpr size_t MAX_INPUT_BYTES = 256;
// Cosine 0.65 on vectors normalized to 127: the "Related" echo threshold.
constexpr int32_t RELATED_MIN_DOT = 10484;

constexpr const char* MODEL_PATH = "/models/platonic-hnet.bin";
constexpr const char* ENGRAM_PATH = "/models/engram.bin";
constexpr const char* EPISODIC_PATH = "/.crosspoint/episodic.idx";
constexpr const char* SHELF_PATH = "/models/inbox-concepts.idx";

// Episodic page numbers: an EPUB position packs (spine + 1) << 16 | page; a
// value below 1 << 16 is a plain page index (TXT).
inline uint32_t epubPosition(const int spineIndex, const int page) {
  const uint32_t spine = spineIndex < 0 ? 0u : static_cast<uint32_t>(spineIndex) + 1u;
  const uint32_t pageBits = page < 0 ? 0u : static_cast<uint32_t>(page > 0xFFFF ? 0xFFFF : page);
  return (spine > 0xFFFF ? 0xFFFF0000u : spine << 16) | pageBits;
}

struct Match {
  uint64_t pathHash = 0;
  uint32_t page = 0;
  int32_t dot = 0;      // int8 dot product; cosine = dot / (127 * 127)
  bool shelf = false;   // from inbox-concepts.idx rather than episodic.idx
  std::string label;
};

uint64_t fnv1a64(std::string_view bytes);

// Encodes up to the first 256 bytes of `utf8Text`. Returns true when the
// PLHN model produced the vector, false when the SimHash fallback did (the
// vector is valid either way; empty text gives all zeros).
bool encodeText(std::string_view utf8Text, int8_t outVec[DIM], const char* engramPath = ENGRAM_PATH);
// The no-model projection, exposed for the shelf builder parity tests.
void encodeFallback(std::string_view utf8Text, int8_t outVec[DIM]);
int32_t dot(const int8_t a[DIM], const int8_t b[DIM]);

// The definition with its best sense for `contextSentence` moved to the top
// and prefixed with `marker`. Senses are "N. ..." lines (numbering may restart
// per part of speech) or, without numbered lines, "; "-separated glosses under
// a header line (cedict). Fewer than two senses: returned unchanged.
std::string rankDictionarySenses(std::string_view contextSentence, std::string_view rawDefinition,
                                 std::string_view marker = "\xE2\x98\x85 ");
std::string rankDictionarySenses(const int8_t contextVec[DIM], std::string_view rawDefinition,
                                 std::string_view marker = "\xE2\x98\x85 ");
// Text of the best sense only (no number), or empty with fewer than two.
std::string bestDictionarySense(const int8_t contextVec[DIM], std::string_view plainDefinition);

// Longest CJK compound tried as an Engram gloss key.
constexpr size_t GLOSS_MAX_CJK_CHARS = 8;

// The Engram gloss for `text`, keyed like the trainer: surrounding whitespace
// and punctuation trimmed, lowercased unless it holds CJK. A CJK run is tried
// as its longest prefix first, down to `minCjkChars` characters. The gloss is
// shown only when the slot holds exactly that span's hash; empty otherwise
// (absent file, empty slot, collision). `matchedWord` receives the key found.
std::string lookupEngramGloss(std::string_view text, std::string* matchedWord = nullptr, size_t minCjkChars = 1,
                              const char* engramPath = ENGRAM_PATH);

// Records a page (or the passage around a lookup) in episodic.idx. A record
// for the same book and page is replaced; past the cap the oldest is reused.
// timestamp_or_flags holds a logical sequence number (newest = largest), so
// the order survives a device without a set clock.
bool appendEpisodicPage(std::string_view bookPath, uint32_t pageIndex, std::string_view pageText);
bool appendEpisodicVector(std::string_view bookPath, uint32_t pageIndex, const int8_t vec[DIM]);

// Best matches across episodic.idx and inbox-concepts.idx, highest first.
std::vector<Match> queryEpisodicAndShelf(std::string_view queryText, size_t maxResults = 3);
std::vector<Match> queryEpisodicAndShelf(const int8_t queryVec[DIM], size_t maxResults = 3);

// Vector of the most recently recorded episodic page.
bool latestEpisodicVector(int8_t outVec[DIM]);
// For each path hash, the dot of its inbox-concepts.idx vector with `query`;
// INT32_MIN where the shelf has no record. Returns how many were found.
size_t scoreShelf(const int8_t queryVec[DIM], const uint64_t* pathHashes, size_t count, int32_t* outDots);

// "<label> p.<N>" (episodic, "ch.<S> p.<N>" for EPUB positions) or "<label>"
// (shelf).
std::string describeMatch(const Match& match);

// Storage locations; tests point them at a scratch directory. Changing the
// model path drops the loaded weights.
struct Paths {
  const char* model = MODEL_PATH;
  const char* episodic = EPISODIC_PATH;
  const char* shelf = SHELF_PATH;
};
void setPaths(const Paths& paths);
// Frees the PSRAM weights (they reload on the next encode).
void unloadModel();

}  // namespace ConceptEngine
