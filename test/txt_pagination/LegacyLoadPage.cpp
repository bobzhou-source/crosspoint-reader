#include <stdlib.h>

#include <algorithm>

#include "LegacyReceiver.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // the legacy page input bound
}  // namespace

#define LOG_ERR(tag, ...) ((void)0)
#define TxtReaderActivity LegacyReceiver
#include "legacy/baseline_load_page.inc"
#undef TxtReaderActivity
#undef LOG_ERR
