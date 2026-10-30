#pragma once

#include "EpubReaderActivity.h"

class HtmlReaderActivity final : public EpubReaderActivity {
 protected:
  std::string prepareBookPath() override;

 public:
  explicit HtmlReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                              bool allowFastInitialRefresh)
      : EpubReaderActivity(renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
};
