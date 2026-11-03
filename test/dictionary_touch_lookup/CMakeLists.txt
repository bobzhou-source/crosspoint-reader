add_executable(DictionaryTouchLookupTest DictionaryTouchLookupTest.cpp ${REPO_ROOT}/src/util/Dictionary.cpp)
target_include_directories(DictionaryTouchLookupTest PRIVATE
  ${REPO_ROOT}/test/cjk_dictionary/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/EpdFont
)
target_link_libraries(DictionaryTouchLookupTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(DictionaryTouchLookupTest)
