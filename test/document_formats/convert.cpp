#include <Fb2.h>
#include <Markdown.h>
#ifdef DOCUMENT_FORMAT_HTML
#include <Html.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const std::string source = argv[1];
  const std::string cache = argv[2];
  const size_t dot = source.find_last_of('.');
  std::string extension = dot == std::string::npos ? "" : source.substr(dot);
  for (char& c : extension) c = std::tolower(static_cast<unsigned char>(c));
  std::string result;
  if (extension == ".fb2") result = Fb2::prepare(source, cache);
  else if (extension == ".md" || extension == ".markdown") result = Markdown::prepare(source, cache);
#ifdef DOCUMENT_FORMAT_HTML
  else if (extension == ".html" || extension == ".htm") result = Html::prepareEpub(source, cache);
#endif
  if (result.empty()) return 1;
  std::printf("%s\n", result.c_str());
  return 0;
}
