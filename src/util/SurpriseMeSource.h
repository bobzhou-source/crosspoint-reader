#pragma once

#include <string>

namespace surprise {

// Finds one item to open for "Surprise me": an unread Inbox article first, then
// an in-progress short read, then an unread Library book. Returns false when
// the card holds nothing eligible. Never builds reading caches.
bool findBook(std::string& outPath);

}  // namespace surprise
