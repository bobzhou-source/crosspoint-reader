# Exercise the real constructor and cache-removal code without linking the EPUB
# parser/image-decoder stack. Regenerate whenever the production bodies change.
set(EPUB_SOURCE "${REPO_ROOT}/lib/Epub/Epub.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EPUB_SOURCE}")
file(READ "${EPUB_SOURCE}" EPUB_SOURCE_TEXT)
string(FIND "${EPUB_SOURCE_TEXT}" "bool Epub::clearCache() const {" CACHE_METHODS_START)
string(FIND "${EPUB_SOURCE_TEXT}" "const std::string& Epub::getTitle() const {" CACHE_METHODS_END)
if(CACHE_METHODS_START LESS 0 OR CACHE_METHODS_END LESS_EQUAL CACHE_METHODS_START)
  message(FATAL_ERROR "Epub cache method boundaries changed; update the focused host source slice")
endif()
math(EXPR CACHE_METHODS_LENGTH "${CACHE_METHODS_END} - ${CACHE_METHODS_START}")
string(SUBSTRING "${EPUB_SOURCE_TEXT}" ${CACHE_METHODS_START} ${CACHE_METHODS_LENGTH} CACHE_METHODS)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/EpubCacheMethods.cpp"
  "// Generated verbatim from lib/Epub/Epub.cpp; do not edit.\n#include <Epub.h>\n#include <Logging.h>\n${CACHE_METHODS}")

add_executable(EpubCacheIdentityTest
  EpubCacheIdentityTest.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/EpubCacheMethods.cpp"
  ${REPO_ROOT}/src/util/BookCacheUtils.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(EpubCacheIdentityTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/test/epub_metadata/stubs
  # Print/Logging only: real Epub/FsHelpers paths above must precede this directory.
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
)

target_link_libraries(EpubCacheIdentityTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(EpubCacheIdentityTest)
