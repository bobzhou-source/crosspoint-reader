#pragma once

#include <stdexcept>
#include <string>

// BookCacheUtils is compiled unchanged. TXT dispatch is outside this EPUB test.
class Txt {
 public:
  Txt(const std::string&, const std::string&) { throw std::logic_error("Unexpected TXT cache dispatch"); }
  bool clearCache() const { throw std::logic_error("Unexpected TXT cache removal"); }
};
