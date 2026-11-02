add_executable(AdaptiveRefreshPolicyTest
  AdaptiveRefreshPolicyTest.cpp
  RefreshPolicyConfigTest.cpp
  ${REPO_ROOT}/lib/AdaptiveRefresh/AdaptiveRefreshPolicy.cpp
  ${REPO_ROOT}/lib/AdaptiveRefresh/RefreshPolicyConfig.cpp
)

target_include_directories(AdaptiveRefreshPolicyTest PRIVATE
  ${REPO_ROOT}/lib/AdaptiveRefresh
  ${REPO_ROOT}/freeink-sdk/libs/display/FreeInkDisplay/include
  ${REPO_ROOT}/.pio/libdeps/x4pro-gh_release/ArduinoJson/src
)

target_link_libraries(AdaptiveRefreshPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AdaptiveRefreshPolicyTest)
