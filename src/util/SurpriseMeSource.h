#pragma once

#include <cstdint>
#include <string>

namespace surprise {

// Finds one item to open for "Surprise me": an unread Inbox article first, then
// an in-progress short read, then an unread Library book. Returns false when
// the card holds nothing eligible. Never builds reading caches.
//
// When /models/inbox-concepts.idx is on the card, Inbox items are scored
// against `currentContextVec` (an int8[128] ConceptEngine vector; null = the
// page last read at length this session, else the page recorded last in the
// episodic index) and a close match is preferred
// within its bucket.
bool findBook(std::string& outPath, const int8_t* currentContextVec = nullptr);

}  // namespace surprise
