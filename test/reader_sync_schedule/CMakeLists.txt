add_executable(ReaderSyncScheduleTest
  ReaderSyncScheduleTest.cpp
)
target_include_directories(ReaderSyncScheduleTest PRIVATE
  ${REPO_ROOT}/src/network
)
target_link_libraries(ReaderSyncScheduleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(ReaderSyncScheduleTest)
