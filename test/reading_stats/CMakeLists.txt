add_executable(reading_stats_test ReadingStatsTest.cpp ${REPO_ROOT}/src/ReadingStatsCore.cpp)
target_include_directories(reading_stats_test PRIVATE ${REPO_ROOT}/src)
target_compile_options(reading_stats_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME reading_stats COMMAND reading_stats_test)
add_executable(reading_stats_runtime_test ReadingStatsRuntimeTest.cpp
  ${REPO_ROOT}/src/ReadingStats.cpp ${REPO_ROOT}/src/ReadingStatsCore.cpp)
target_include_directories(reading_stats_runtime_test PRIVATE stubs ${REPO_ROOT}/src)
target_compile_options(reading_stats_runtime_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME reading_stats_runtime COMMAND reading_stats_runtime_test)
