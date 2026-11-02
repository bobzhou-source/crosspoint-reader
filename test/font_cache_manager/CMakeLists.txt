add_executable(FontCacheManagerTest
  FontCacheManagerTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(FontCacheManagerTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(FontCacheManagerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FontCacheManagerTest)

# Vector (TTF) variant. The target above compiles the TTF path out (stubs/TtfEpdFont.h,
# CROSSPOINT_VECTOR_FONTS 0); this one builds the REAL TtfEpdFont/EpdFont/FontCacheManager
# with BOARD_HAS_PSRAM so the vector prewarm lifetime is exercised end to end. Only the
# FreeType engine (FtFont), the heap-size queries and the SDK memory manager are host
# stand-ins (vector_stubs/); vector_stubs/ must stay FIRST so its FontDecompressor.h and
# SdCardFont.h shadow lib/EpdFont's real ones.
add_executable(FontCacheManagerVectorTest
  FontCacheManagerVectorTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/EpdFont/TtfEpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_compile_definitions(FontCacheManagerVectorTest PRIVATE BOARD_HAS_PSRAM=1)

target_include_directories(FontCacheManagerVectorTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/vector_stubs
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(FontCacheManagerVectorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FontCacheManagerVectorTest)
