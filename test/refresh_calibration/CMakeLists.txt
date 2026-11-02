add_executable(RefreshCalibrationLadderTest
  RefreshCalibrationLadderTest.cpp
  RefreshCalibrationTextTest.cpp
  ${REPO_ROOT}/lib/RefreshCalibration/RefreshCalibrationLadder.cpp
  ${REPO_ROOT}/lib/RefreshCalibration/RefreshCalibrationText.cpp
  ${REPO_ROOT}/lib/AdaptiveRefresh/AdaptiveRefreshPolicy.cpp
)

target_include_directories(RefreshCalibrationLadderTest PRIVATE
  ${REPO_ROOT}/lib/RefreshCalibration
  ${REPO_ROOT}/lib/AdaptiveRefresh
  ${REPO_ROOT}/freeink-sdk/libs/display/FreeInkDisplay/include
  ${REPO_ROOT}/.pio/libdeps/x4pro-gh_release/ArduinoJson/src
)

target_link_libraries(RefreshCalibrationLadderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(RefreshCalibrationLadderTest)
