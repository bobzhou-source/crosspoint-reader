add_executable(BenchmarkTest
  BenchmarkTest.cpp
  RefreshClearEventTest.cpp
  NavScriptTest.cpp
  NavRestoreTest.cpp
  NavFeaturesTest.cpp
  ${REPO_ROOT}/lib/Benchmark/BenchmarkBuffer.cpp
  ${REPO_ROOT}/lib/Benchmark/RefreshClearEvent.cpp
  ${REPO_ROOT}/lib/AdaptiveRefresh/AdaptiveRefreshPolicy.cpp
  ${REPO_ROOT}/lib/Benchmark/NavScript.cpp
  ${REPO_ROOT}/lib/Benchmark/NavRestore.cpp
  ${REPO_ROOT}/lib/Benchmark/NavFeatures.cpp
)
target_include_directories(BenchmarkTest PRIVATE
  ${REPO_ROOT}/lib/Benchmark
  ${REPO_ROOT}/lib/AdaptiveRefresh
  ${REPO_ROOT}/freeink-sdk/libs/display/FreeInkDisplay/include
)
target_link_libraries(BenchmarkTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(BenchmarkTest)
