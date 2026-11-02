add_executable(TxtPaginationTest
  TxtPaginationTest.cpp
  TxtPageIndexFormatTest.cpp
  TxtChaptersTest.cpp
  TxtBackgroundIndexingTest.cpp
  LegacyLoadPage.cpp
  AllocationCounter.cpp
  ${REPO_ROOT}/lib/Txt/TxtPagination.cpp
  ${REPO_ROOT}/lib/Txt/TxtChapters.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(TxtPaginationTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Txt
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/BenchmarkFixtures
)

target_link_libraries(TxtPaginationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TxtPaginationTest)
