#include "MarkdownReaderActivity.h"

#include <I18n.h>
#include <Markdown.h>

#include "components/UITheme.h"

std::string MarkdownReaderActivity::prepareBookPath() {
  disableFastInitialRefresh();
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  return Markdown::prepare(bookPath);
}
