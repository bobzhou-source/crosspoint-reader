#include "Fb2ReaderActivity.h"

#include <Fb2.h>
#include <I18n.h>

#include "components/UITheme.h"

std::string Fb2ReaderActivity::prepareBookPath() {
  disableFastInitialRefresh();
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  return Fb2::prepare(bookPath);
}
