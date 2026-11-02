add_executable(BenchmarkTest
  BenchmarkTest.cpp
  ${REPO_ROOT}/lib/Benchmark/BenchmarkBuffer.cpp
)
target_include_directories(BenchmarkTest PRIVATE
  ${REPO_ROOT}/lib/Benchmark
)
target_link_libraries(BenchmarkTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
gtest_discover_tests(BenchmarkTest)
