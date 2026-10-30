#pragma once

#include <string>

namespace Fb2 {
// Returns a complete cached EPUB path, or an empty string on conversion failure.
// Reader identity and the native EPUB cache must use sourcePath, not this path.
std::string prepare(const std::string& sourcePath, const std::string& cacheRoot = "/.crosspoint");
}
