add_executable(input_latch_tests InputLatchTest.cpp)
target_include_directories(input_latch_tests PRIVATE
  ${REPO_ROOT}/freeink-sdk/libs/hardware/InputManager/include)
target_link_libraries(input_latch_tests PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(input_latch_tests)
