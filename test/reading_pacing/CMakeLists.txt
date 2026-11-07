add_executable(ReadingPacingTest
  ReadingPacingTest.cpp
)

target_link_libraries(ReadingPacingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingPacingTest)
