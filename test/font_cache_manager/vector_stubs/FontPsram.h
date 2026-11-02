#pragma once

// Host stand-in for FreeInkFont's PSRAM-preferring containers: plain heap vectors.
#include <vector>

namespace freeink {
namespace font {

template <typename T>
using PsramVector = std::vector<T>;

}  // namespace font
}  // namespace freeink
