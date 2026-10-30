#pragma once

#include "EpubReaderActivity.h"

class Fb2ReaderActivity final : public EpubReaderActivity {
 protected:
  std::string prepareBookPath() override;

 public:
  Fb2ReaderActivity(GfxRenderer& renderer, MappedInputManager& input, std::string path, bool allowFastInitialRefresh)
      : EpubReaderActivity(renderer, input, std::move(path), allowFastInitialRefresh) {}
};
