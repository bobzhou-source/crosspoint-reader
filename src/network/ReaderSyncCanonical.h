#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace reader_sync {

// Largest native record AnnotationStore writes or accepts (8 KiB).
inline constexpr size_t MAX_CANONICAL_INPUT = 8192;
// One annotation body must leave room for {"annotations":[]} inside the 64 KiB
// cloud batch limit, exactly like companion annotation_batches().
inline constexpr size_t MAX_CANONICAL_RECORD = 64 * 1024 - 18;
// Deepest JSON nesting a native record may use (ArduinoJson's own default).
inline constexpr size_t MAX_CANONICAL_DEPTH = 10;

// Canonicalizes one native annotation file (raw JSON, length bytes) into the
// exact record companion/cloud_annotations.py map_annotation() uploads, plus the
// UUID5 it derives for that record.
//
//   value    = json.loads(raw)                       (source_anchor input)
//   native   = value without "excerpt" and "note"
//   anchor   = json.dumps({format:"reader-workbench-native-v1",
//                          device_id:deviceId, native:native},
//                         ensure_ascii=False, separators=(",",":"), sort_keys=True)
//   stable   = json.dumps([deviceId, None, value], ...same options...)
//   uuid     = uuid5(c5a4683a-f89f-54d3-9b39-4a6db1da194d, stable)
//   record   = {"id":uuid,"item_id":null,"book_path":value["sourcePath"],
//               "source_anchor":anchor,"selection":value.get("excerpt",""),
//               "kind":kind_name(value.get("kind",0)),"note":value.get("note",""),
//               "created_at":value["createdAt"]*1000}   (sorted, compact)
//
// uploadRecord receives the compact sorted UTF-8 JSON of that record; uuid
// receives the lowercase canonical UUID text. Both match the companion byte for
// byte for every native record the companion accepts.
//
// Numbers keep their original JSON type: integer tokens are emitted as Python
// ints (exact decimal, no float rounding) and fractional/exponent tokens become
// Python float repr(). Malformed, out-of-range or oversized input is refused
// with a description in `error`; both outputs stay empty and nothing is
// truncated. nowSeconds is compared against createdAt exactly like
// map_annotation(..., item_id=None, now=nowSeconds). A null deviceId counts as
// an empty string, which the companion rejects.
//
// Host verification (no hardware, no firmware build):
//   python3 test/reader_sync/canonical_oracle.py
//
// Thread-safe: no globals, no heap arena beyond the returned strings.
bool canonicalAnnotation(const char* raw, size_t length, const char* deviceId, int64_t nowSeconds,
                         std::string& uploadRecord, std::string& uuid, std::string& error);

}  // namespace reader_sync
