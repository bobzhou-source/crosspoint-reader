#include "Lyra3CoversTheme.h"

int Lyra3CoversTheme::homeCoverThumbHeight(const GfxRenderer& renderer, int recentBookCount) const {
  return recentBookCount > 1 ? 112 : LyraTheme::homeCoverThumbHeight(renderer, recentBookCount);
}
