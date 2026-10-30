#pragma once

#include <string>

namespace Markdown {
// Returns the cached EPUB transport; keep sourcePath as the reader identity.
std::string prepare(const std::string& sourcePath, const std::string& cacheRoot = "/.crosspoint");
}
