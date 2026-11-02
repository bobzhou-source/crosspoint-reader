add_executable(AdaptiveRefreshPolicyTest
  AdaptiveRefreshPolicyTest.cpp
  ${REPO_ROOT}/lib/AdaptiveRefresh/AdaptiveRefreshPolicy.cpp
)

target_include_directories(AdaptiveRefreshPolicyTest PRIVATE
  ${REPO_ROOT}/lib/AdaptiveRefresh
  ${REPO_ROOT}/freeink-sdk/libs/display/FreeInkDisplay/include
)

target_link_libraries(AdaptiveRefreshPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AdaptiveRefreshPolicyTest)
