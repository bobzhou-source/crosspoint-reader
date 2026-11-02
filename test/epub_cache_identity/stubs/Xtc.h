#pragma once

#include <stdexcept>
#include <string>

// BookCacheUtils is compiled unchanged. XTC dispatch is outside this EPUB test.
class Xtc {
 public:
  Xtc(const std::string&, const std::string&) { throw std::logic_error("Unexpected XTC cache dispatch"); }
  bool clearCache() const { throw std::logic_error("Unexpected XTC cache removal"); }
};
