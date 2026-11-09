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
// - episodic.exc (the episodic path with ".exc" for ".idx"): the excerpt
//   sidecar, no header, 176-byte slots parallel to the episodic.idx records:
//   u64 path hash, u32 page (both must match the record at that index, else
//   the slot is stale), u32 0, char excerpt[160] (NUL-terminated). The
//   episodic.idx layout itself is unchanged.
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
// - pe-writer.bin (PEWR v1, 641,726 bytes) and writer-clauses.bin (WCLS v1,
//   1,048,608 bytes): see generateMarginRiff below.
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
constexpr const char* MODELS_DIR = "/models";
constexpr const char* PE_WRITER_FILE = "pe-writer.bin";
constexpr const char* WRITER_CLAUSES_FILE = "writer-clauses.bin";
constexpr const char* MARGIN_NOTES_PATH = "/.crosspoint/margin-notes.txt";

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
  int8_t vec[DIM] = {};  // the record's concept vector
  // Passage text: the episodic excerpt saved with the page, or for story
  // matches the scene gloss (and its place or target). Empty when unknown.
  std::string excerpt;
  uint32_t index = 0;      // record index in its .idx file (story: event index)
  bool story = false;      // a story-graph.idx scene (page = its chapter_or_page)
  bool crossBook = false;  // story scene with slot_hint 3 (a cross-book echo)
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
// the order survives a device without a set clock. appendEpisodicPage also
// saves excerptAround(pageText) in the episodic.exc sidecar slot of that
// record; appendEpisodicVector leaves the sidecar alone.
bool appendEpisodicPage(std::string_view bookPath, uint32_t pageIndex, std::string_view pageText);
bool appendEpisodicVector(std::string_view bookPath, uint32_t pageIndex, const int8_t vec[DIM]);

// Longest saved episodic excerpt.
constexpr size_t EXCERPT_BYTES = 150;
// `text` with whitespace runs collapsed to one space, cut to at most
// `maxBytes` around its middle (the held word, for a lookup's context) on
// word boundaries (code point boundaries for unspaced text), with "…" marking
// each cut side.
std::string excerptAround(std::string_view text, size_t maxBytes = EXCERPT_BYTES);

// Best matches across episodic.idx and, with `includeShelf`, inbox-concepts.idx,
// highest first. Episodic matches carry their sidecar excerpt when one is saved.
std::vector<Match> queryEpisodicAndShelf(std::string_view queryText, size_t maxResults = 3);
std::vector<Match> queryEpisodicAndShelf(const int8_t queryVec[DIM], size_t maxResults = 3, bool includeShelf = true);

// Vector of the most recently recorded episodic page.
bool latestEpisodicVector(int8_t outVec[DIM]);
// For each path hash, the dot of its inbox-concepts.idx vector with `query`;
// INT32_MIN where the shelf has no record. Returns how many were found.
size_t scoreShelf(const int8_t queryVec[DIM], const uint64_t* pathHashes, size_t count, int32_t* outDots);

// ---- Reading context -----------------------------------------------------------

// A page left after CONTEXT_DWELL_MS becomes the current reading context; one
// left after EPISODE_DWELL_MS is also recorded in episodic.idx. Both use the
// page's first DWELL_SLICE_BYTES of text. The encode waits until the next
// page has stayed up DWELL_SETTLE_MS, so a quick follow-up turn goes first.
constexpr uint32_t CONTEXT_DWELL_MS = 10000;
constexpr uint32_t EPISODE_DWELL_MS = 20000;
constexpr uint32_t DWELL_SETTLE_MS = 400;
constexpr size_t DWELL_SLICE_BYTES = 200;

// The in-memory reading context (RAM only: gone after a reboot). Main task.
void setCurrentContext(const int8_t vec[DIM]);
bool currentContext(int8_t outVec[DIM]);
void clearCurrentContext();
// The current context, else the newest episodic page: what Surprise Me and
// Quick Break rank the Inbox shelf against.
bool readingContext(int8_t outVec[DIM]);

// Main task: the reader left page `pageIndex` (an episodic position) of
// `bookPath` after `dwellMs`. Encodes `slice` into the current context from
// CONTEXT_DWELL_MS and appends it to episodic.idx (with its excerpt) from
// EPISODE_DWELL_MS. False when the dwell was too short or the slice empty
// (nothing changed).
bool recordPageDwell(std::string_view bookPath, uint32_t pageIndex, std::string_view slice, uint32_t dwellMs);

// Hands pages read at length from a reader's render task to its main task,
// with no allocation or I/O on the render side. Not synchronized itself: both
// sides hold the activity's render lock.
class PageDwellQueue {
 public:
  struct Page {
    uint32_t index = 0;    // episodic position (epubPosition, or a TXT page index)
    uint32_t dwellMs = 0;  // how long it stayed on the panel
    uint16_t length = 0;
    char text[DWELL_SLICE_BYTES] = {};
    // Empty when the render had no page text at hand (a prerendered body).
    std::string_view slice() const { return {text, length}; }
    // Appends to the slice, cut on a code point boundary at
    // DWELL_SLICE_BYTES. False once the slice is full.
    bool append(std::string_view more);
  };

  // Render task: page `index` just went up, `dwellMs` after the frame before
  // it. Queues the page that frame showed when it stayed CONTEXT_DWELL_MS or
  // longer (replacing an older queued one), then returns the new page with an
  // empty slice to fill.
  Page& frameShown(uint32_t dwellMs, uint32_t index, uint32_t nowMs);
  // Main task: moves out the queued page once the frame after it has stayed
  // up `settleMs`. False when nothing is queued or it is too soon.
  bool take(Page& out, uint32_t nowMs, uint32_t settleMs);

 private:
  Page shown;
  Page queued;
  uint32_t shownAtMs = 0;
  bool hasShown = false;
  bool hasQueued = false;
};

// The longest prefix of `text` of at most `maxBytes` that does not split a
// UTF-8 code point.
std::string_view utf8Prefix(std::string_view text, size_t maxBytes);

// "<label> p.<N>" (episodic, "ch.<S> p.<N>" for EPUB positions), "<label>"
// (shelf), or "<label> Ch.<N>" (story scene; "<label>" when N is 0).
std::string describeMatch(const Match& match);

// "Related" passages from episodic, shelf and story matches, best first.
// Compact: the first RELATED_COMPACT_ITEMS matches, one line each of at most
// RELATED_COMPACT_LINE_BYTES: where, then a quoted excerpt shortened on a word
// boundary. Extended: up to RELATED_EXTENDED_ITEMS items, each a "• where"
// line and its whole excerpt, separated by blank lines, at most
// RELATED_EXTENDED_BYTES (items that do not fit are left out whole).
constexpr size_t RELATED_COMPACT_ITEMS = 2;
constexpr size_t RELATED_COMPACT_LINE_BYTES = 140;
constexpr size_t RELATED_EXTENDED_ITEMS = 4;
constexpr size_t RELATED_EXTENDED_BYTES = 1200;
std::string formatRelatedNotes(const std::vector<Match>& matches, bool extended);

// ---- CS-PW story refresher ---------------------------------------------------

// A spoiler-safe "Story: ..." refresher for `word` in `bookPath` at
// `currentCutoff` (epubStoryCutoff / txtStoryCutoff; 0 = ungated), written
// NUL-terminated into outStory (cut on a UTF-8 boundary). False when the word
// is no story-graph entity, its alias is not yet revealed, it is introduced
// after the cutoff, no event precedes the cutoff, or story-graph.idx is
// absent. Without cspw.bin it still writes the refresher with default frames.
bool weaveEntityRefresher(const char* word, size_t wordLen, const char* bookPath, uint32_t currentCutoff,
                          char* outStory, size_t outStoryCap);

// The longer "story so far" dossier for the same entity and gates: its name
// line, then one "• Ch.<N>: [<hint>] <gloss> -> <target>" line per scene
// (hint: role, origin, arc; "Ch." without a chapter, the target when empty
// or already in the gloss are left out) in chronological order, from its
// first appearance to its latest scene at or before `currentCutoff` (never a
// later one). With more than STORY_TIMELINE_SCENES eligible scenes it keeps
// the first, the latest and the most salient ones between them. Cross-book
// echoes are left to queryStoryEchoes (the timeline uses them only when the
// entity has no in-book scene). A line that does not fit `outStoryCap` is
// left out whole.
constexpr size_t STORY_EXTENDED_BYTES = 1536;
constexpr size_t STORY_TIMELINE_SCENES = 6;
bool weaveExtendedRefresher(const char* word, size_t wordLen, const char* bookPath, uint32_t currentCutoff,
                            char* outStory, size_t outStoryCap);

// Spoiler-safe story-graph scenes for "Related" passages, highest first:
// first the newest cross-book echo (slot_hint 3) of `focusWord`'s entity at or
// before the cutoff (dot = INT32_MAX: pinned ahead of scored matches), then
// scenes of other entities of `bookPath` that are introduced and happen at or
// before `currentCutoff` with a concept dot of at least RELATED_MIN_DOT
// against `queryVec`. The focus entity's own in-book scenes are its Story and
// are left out; other books' entities are never read (the reader's place in
// them is unknown). Reads at most STORY_ECHO_SCAN_EVENTS x 256 bytes of
// entity rows and events. Empty without story-graph.idx.
constexpr size_t STORY_ECHO_SCAN_EVENTS = 512;
std::vector<Match> queryStoryEchoes(const int8_t queryVec[DIM], const char* bookPath, uint32_t currentCutoff,
                                    std::string_view focusWord, size_t maxResults = RELATED_EXTENDED_ITEMS);

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

// ---- PE-Writer margin riffs ----------------------------------------------------

constexpr size_t PE_PLAN_STEPS = 3;
constexpr size_t PE_MAX_TOKENS = 36;
// Longest riff, cut on a code point boundary.
constexpr size_t RIFF_MAX_BYTES = 239;
// margin-notes.txt keeps the newest this many notes, one per line.
constexpr size_t MARGIN_NOTES_MAX = 64;

// A 25-45 word analytical "margin riff" about the page whose concept vector is
// `cPage`, conditioned on a cross-book echo `cEcho` (nullptr = none), from
// <modelsDir>/pe-writer.bin (PEWR v1: 3-step looped latent thought planner,
// 64-bucket centroid router, gated ShortConv(k=3) micro-decoder with a
// clause-copy boost) and <modelsDir>/writer-clauses.bin (WCLS v1: 64 buckets of
// 64 x 256-byte analytical clause slots, of which only the 3 routed buckets are
// read). The weights are read into PSRAM for this call only. `subject`, `role`
// and `echoTitle` (nullptr = empty) fill the <ENT_SUBJECT>, <ENT_ROLE> ("this
// concept" when empty) and <BOOK_ECHO> tokens. The optional outputs get the
// thought vectors, bucket ids and token ids (0 after the end). False when a
// file is missing or malformed, memory runs out, or the riff is empty.
//
// PE-Reasoner v3: with `pageContext` (the page text around the word; the first
// PE_PAGE_CONTEXT_MAX bytes are read) the writer first points into the page.
// The context splits into at most 16 clause segments (. ! ? ; : and their CJK
// forms); every window of 4-12 words (or up to 24 CJK characters) is scored by
// subject / role overlap, nearness to the subject, content words and its
// span-lexicon vector against thought 0, and the best window's page words
// (quotes and brackets dropped) become the evidence clause. Each step then
// reads the two best-centroid buckets of its role partition and takes the slot
// with the best dot(slot, plan) - dot(slot, hub) / 2 (hub = the normalised sum
// of the centroids), penalising a word 2-gram shared with the evidence or an
// earlier clause. The riff is "<subject> (<role>): <evidence>; <in | while in |
// much as in> <echoTitle>, <clause 0>, <clause 1>." (clauses, then the echo,
// dropped to fit RIFF_MAX_BYTES), the connector from the decoder's relation
// token. Without context, or without a window, the riff is the v2 one above.
constexpr size_t PE_PAGE_CONTEXT_MAX = 1024;
bool generateMarginRiff(const std::string& modelsDir, const int8_t cPage[DIM], const int8_t cEcho[DIM],
                        const char* subject, const char* role, const char* echoTitle, std::string& outRiff,
                        int8_t outPlan[PE_PLAN_STEPS][DIM] = nullptr, uint8_t outBuckets[PE_PLAN_STEPS] = nullptr,
                        uint16_t outTokens[PE_MAX_TOKENS] = nullptr, std::string_view pageContext = {});

// The longer riff: three complete sentences, one per thought step s = 0..2,
// at most RIFF_EXTENDED_MAX_BYTES. Step s routes to its two best-centroid
// buckets and takes its two best clauses from them that no earlier sentence
// used (clause ids and texts are never repeated). Sentence 1 anchors on
// `subject` and `role`, sentence 2 on `echoTitle` (the context when empty),
// sentence 3 states the tension; every analytical phrase is a
// writer-clauses.bin clause verbatim (trailing punctuation and broken UTF-8
// tails trimmed), and each sentence ends in "." ("。" after CJK). A sentence
// that would pass the cap is left out whole. With `outCompactRiff` the same
// call also writes exactly what generateMarginRiff would (one model load for
// both). `outClauseIds` gets bucket * 64 + slot per used clause, in order,
// 0xFFFF for unused entries. False when a file is missing or malformed,
// memory runs out, or no clause is available. With an evidence clause from
// `pageContext` (see generateMarginRiff) sentence 1 is "<lead>: <evidence>;
// <clause>.", sentence 2 opens with a second hop (the best window of another
// segment sharing a word with the subject or the evidence) when there is one,
// and the clauses come from the role-partition anti-hub ranking.
constexpr size_t RIFF_EXTENDED_MAX_BYTES = 640;
constexpr size_t RIFF_EXTENDED_CLAUSES = 2 * PE_PLAN_STEPS;
bool generateExtendedMarginRiff(const std::string& modelsDir, const int8_t cPage[DIM], const int8_t cEcho[DIM],
                                const char* subject, const char* role, const char* echoTitle, std::string& outRiff,
                                std::string* outCompactRiff = nullptr,
                                uint16_t outClauseIds[RIFF_EXTENDED_CLAUSES] = nullptr,
                                std::string_view pageContext = {});

// Appends `note` (newlines flattened, cut at 480 bytes) as the newest line of
// `path`, dropping the oldest lines past MARGIN_NOTES_MAX. The file is
// rewritten through `<path>.tmp`.
bool appendMarginNote(std::string_view note, const char* path = MARGIN_NOTES_PATH);

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
