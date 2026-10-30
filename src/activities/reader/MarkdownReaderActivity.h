#pragma once

#include "EpubReaderActivity.h"

class MarkdownReaderActivity final : public EpubReaderActivity {
 protected:
  std::string prepareBookPath() override;

 public:
  MarkdownReaderActivity(GfxRenderer& renderer, MappedInputManager& input, std::string path, bool allowFastInitialRefresh)
      : EpubReaderActivity(renderer, input, std::move(path), allowFastInitialRefresh) {}
};
