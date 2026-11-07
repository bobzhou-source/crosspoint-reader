add_executable(SurpriseMeTest SurpriseMeTest.cpp ${REPO_ROOT}/src/util/SurpriseMe.cpp)
target_include_directories(SurpriseMeTest PRIVATE ${REPO_ROOT}/src)
target_link_libraries(SurpriseMeTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(SurpriseMeTest)
