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
// Engram lookups per H-Net chunk: the chunk's own span (unigram) and, when it
// and the chunk before it total at most 18 bytes, their joined span (bigram,
// exact span-hash match only, half-weight delta), so a 4-character idiom split
// across two chunks still hits its slot.
//
// Story refreshers (CS-PW v2): /models/cspw.bin (CSPW v2, 393,728 bytes) is the
// Chrono-Slot Pointer Weaver (up_proj -> causal 4-head Chunk-GQA -> down_proj
// -> salience -> 4-slot Chrono-CfC -> frame/phrase pointer decoder), loaded on
// demand into PSRAM next to the encoder; /models/story-graph.idx (SGRF v1) is
// the spoiler-gated entity/event/alias index it reads. Every number the weaver
// prints comes from story-graph events at or before the reader's cutoff.
//
// Binary formats (all little-endian):
// - engram.bin: 512-byte header sector ("ENGR", version 1, num_slots,
//   slot_bytes = 64, float scale = 2^-Sg), then 64-byte slots: u64 FNV-1a 64
//   span hash (0 = empty), int8 concept_delta[24], char gloss[32].
// - episodic.idx / inbox-concepts.idx: 16-byte header ("ICPT", version 1,
//   count, dim = 128), then 192-byte records: u64 FNV-1a 64 of the path,
//   u32 page_or_chunk, u32 timestamp_or_flags, char label[48], int8 vec[128].
// - cspw.bin: 512-byte header ("CSPW", u16 version 2, u16 flags, u32 d_model
//   = 192, d_slot = 128, n_slots = 4, n_frames = 256, n_phrases = 512, u32 0,
//   float scales[16], zero pad), then row-major int8 up_proj[128][192],
//   gqa_q[192][192], gqa_k[192][96], gqa_v[192][96], gqa_o[192][192],
//   down_proj[192][128], salience_w[128] + i16 salience_b (128-byte block),
//   slot_gate_w[4][128 out][128 in], frame_bank[256][128],
//   char frame_strings[256][64], phrase_bank[512][128],
//   char phrase_strings[512][100], zero tail to 393,728 bytes.
// - story-graph.idx: 512-byte header ("SGRF", u16 version 1, u16 flags, u32
//   num_entities, num_events, num_aliases, entities_offset, events_offset,
//   aliases_offset); 64-byte entities sorted by (term_hash, book_path_hash):
//   u64 term_hash, u64 book_path_hash (0 = any book), u32 first_cutoff, u32
//   event_start_idx, u16 event_count, u8 kind, u8 flags, char
//   en_name_or_role[32], char book_short[4]; 256-byte events, ascending
//   cutoff per entity: u32 cutoff, u16 chapter_or_page, u8 slot_hint
//   (0 role, 1 origin, 2 arc, 3 cross-book), u8 salience_hint, char
//   event_gloss[64], char target_or_loc[56], int8 vec[128]; 24-byte aliases
//   sorted by alias_hash: u64 alias_hash, u64 canonical_hash, u32
//   reveal_cutoff, u32 reserved.
namespace ConceptEngine {

constexpr size_t DIM = 128;
constexpr size_t MAX_INPUT_BYTES = 256;
// Cosine 0.65 on vectors normalized to 127: the "Related" echo threshold.
constexpr int32_t RELATED_MIN_DOT = 10484;

constexpr const char* MODEL_PATH = "/models/platonic-hnet.bin";
constexpr const char* ENGRAM_PATH = "/models/engram.bin";
constexpr const char* EPISODIC_PATH = "/.crosspoint/episodic.idx";
constexpr const char* SHELF_PATH = "/models/inbox-concepts.idx";
constexpr const char* CSPW_PATH = "/models/cspw.bin";
constexpr const char* STORY_GRAPH_PATH = "/models/story-graph.idx";

// Episodic page numbers: an EPUB position packs (spine + 1) << 16 | page; a
// value below 1 << 16 is a plain page index (TXT).
inline uint32_t epubPosition(const int spineIndex, const int page) {
  const uint32_t spine = spineIndex < 0 ? 0u : static_cast<uint32_t>(spineIndex) + 1u;
  const uint32_t pageBits = page < 0 ? 0u : static_cast<uint32_t>(page > 0xFFFF ? 0xFFFF : page);
  return (spine > 0xFFFF ? 0xFFFF0000u : spine << 16) | pageBits;
}

// Story-graph spoiler cutoffs, independent of font and layout:
// (spine + 1) << 16 | visible-codepoint offset / 64 for EPUB, and
// 1 << 16 | byte offset / 1024 for TXT. 0 means "no cutoff" (all revealed).
inline uint32_t epubStoryCutoff(const int spineIndex, const uint32_t visibleTextOffset) {
  const uint32_t spine = spineIndex < 0 ? 0u : static_cast<uint32_t>(spineIndex) + 1u;
  const uint32_t chunk = visibleTextOffset >> 6;
  return (spine > 0xFFFF ? 0xFFFF0000u : spine << 16) | (chunk > 0xFFFF ? 0xFFFFu : chunk);
}
inline uint32_t txtStoryCutoff(const size_t byteOffset) {
  const size_t chunk = byteOffset >> 10;
  return (1u << 16) | static_cast<uint32_t>(chunk > 0xFFFF ? 0xFFFF : chunk);
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

// The Engram gloss for `word` when its slot holds exactly that span; empty
// otherwise (absent file, empty slot, hash collision).
std::string lookupEngramGloss(std::string_view word, const char* engramPath = ENGRAM_PATH);

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

// ---- CS-PW story refresher ---------------------------------------------------

// A spoiler-safe "Story: ..." refresher for `word` in `bookPath` at
// `currentCutoff` (epubStoryCutoff / txtStoryCutoff; 0 = ungated), written
// NUL-terminated into outStory (cut on a UTF-8 boundary). False when the word
// is no story-graph entity, its alias is not yet revealed, it is introduced
// after the cutoff, no event precedes the cutoff, or story-graph.idx is
// absent. Without cspw.bin it still writes the refresher with default frames.
bool weaveEntityRefresher(const char* word, size_t wordLen, const char* bookPath, uint32_t currentCutoff,
                          char* outStory, size_t outStoryCap);

constexpr size_t CSPW_SLOTS = 4;
constexpr size_t CSPW_MAX_SCENES = 4;

// The integer Chrono-CfC state for one entity, from runChronoSlots.
struct ChronoSlots {
  int8_t slot[CSPW_SLOTS][DIM] = {};  // 0 Role, 1 Origin, 2 Recent Arc, 3 Cross-Book Echo
  uint8_t salience[CSPW_MAX_SCENES] = {};
  uint16_t tau[CSPW_MAX_SCENES] = {};  // half-life in pages
  int16_t frame[CSPW_SLOTS] = {-1, -1, -1, -1};  // best frame per slot group, -1 = none
  int16_t phrase = -1;                            // best phrase for Slot 0, -1 = none
  int16_t arcScene = -1;                          // scene behind %A, -1 with one scene
};

// The CS-PW kernel over `count` (1..4) chronological scene vectors at pages
// `pages`, with an optional cross-book echo vector. False without a loaded
// cspw.bin or with a bad count.
bool runChronoSlots(const int8_t (*scenes)[DIM], const uint32_t* pages, size_t count, const int8_t* echo,
                    ChronoSlots& out);
// Half-life in pages for salience g: max(16, round(2^((64 + g/2) / 16))).
uint16_t cspwHalfLife(uint8_t salience);
// round(32768 * 2^(-deltaPages / tau)) in Q15 through the 17-entry LUT.
uint16_t cspwDecayQ15(uint32_t deltaPages, uint32_t tau);

// Storage locations; tests point them at a scratch directory. Changing the
// model or CS-PW path drops the loaded weights.
struct Paths {
  const char* model = MODEL_PATH;
  const char* episodic = EPISODIC_PATH;
  const char* shelf = SHELF_PATH;
  const char* cspw = CSPW_PATH;
  const char* storyGraph = STORY_GRAPH_PATH;
};
void setPaths(const Paths& paths);
// Frees the PSRAM weights of both models (they reload on next use).
void unloadModel();

}  // namespace ConceptEngine
