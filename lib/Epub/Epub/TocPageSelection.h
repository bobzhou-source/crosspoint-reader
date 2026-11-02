#pragma once

#include <optional>

// Unknown anchors never supersede the spine's fallback. Equal-page anchors
// select the later TOC entry (the more specific nested heading).
class TocPageSelection {
  int index;
  int latestPage = -1;
  const int currentPage;

 public:
  TocPageSelection(int fallbackIndex, int currentPage) : index(fallbackIndex), currentPage(currentPage) {}
  void consider(int tocIndex, std::optional<unsigned> page) {
    if (page && static_cast<int>(*page) <= currentPage && static_cast<int>(*page) >= latestPage) {
      latestPage = static_cast<int>(*page);
      index = tocIndex;
    }
  }
  int selected() const { return index; }
};
