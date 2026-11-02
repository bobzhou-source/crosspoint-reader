add_executable(EpubMetadataTest EpubMetadataTest.cpp)
target_include_directories(EpubMetadataTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Epub
)
target_link_libraries(EpubMetadataTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(EpubMetadataTest)
