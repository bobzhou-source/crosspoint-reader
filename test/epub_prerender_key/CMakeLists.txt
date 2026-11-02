add_executable(EpubPrerenderKeyTest
  EpubPrerenderKeyTest.cpp
)

target_include_directories(EpubPrerenderKeyTest PRIVATE
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(EpubPrerenderKeyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(EpubPrerenderKeyTest)
