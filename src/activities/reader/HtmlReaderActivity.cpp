#include "HtmlReaderActivity.h"

#include <Html.h>

std::string HtmlReaderActivity::prepareBookPath() { return Html::prepareEpub(bookPath); }
