set(TEXT_LAYOUT_SOURCES
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/WordStore.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

add_executable(KoreanLineBreakingTest KoreanLineBreakingTest.cpp ${TEXT_LAYOUT_SOURCES})
add_executable(LayoutDump LayoutDump.cpp ${TEXT_LAYOUT_SOURCES})
add_executable(TextLayoutTest TextLayoutTest.cpp ${TEXT_LAYOUT_SOURCES})

foreach(target KoreanLineBreakingTest LayoutDump TextLayoutTest)

target_include_directories(${target} PRIVATE
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/FsHelpers
)

target_link_libraries(${target} PRIVATE crosspoint_test_common)
endforeach()

target_link_libraries(KoreanLineBreakingTest PRIVATE GTest::gtest_main)
gtest_discover_tests(KoreanLineBreakingTest)
target_link_libraries(TextLayoutTest PRIVATE GTest::gtest_main)
gtest_discover_tests(TextLayoutTest)

# Optional evidence probe using the firmware's real font backend. The normal
# behavior tests above deliberately use deterministic, device-independent metrics.
option(TEXT_LAYOUT_REAL_FONT_PROBE "Build actual TTF advance probe" OFF)
if(TEXT_LAYOUT_REAL_FONT_PROBE)
  set(FONT_ROOT ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont)
  add_executable(FontAdvanceDump
    FontAdvanceDump.cpp
    ${REPO_ROOT}/lib/EpdFont/TtfEpdFont.cpp
    ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
    ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
    ${REPO_ROOT}/lib/Utf8/Utf8.cpp
    ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
    ${REPO_ROOT}/lib/MiniBidi/minibidi.c
    ${FONT_ROOT}/src/FtFont.cpp
    ${FONT_ROOT}/src/Gsub.cpp
    ${FONT_ROOT}/src/Gpos.cpp
    ${FONT_ROOT}/src/FontAlloc.c
    ${FONT_ROOT}/src/freetype/ft_psnames.c
    ${FONT_ROOT}/src/freetype/ft_smooth.c
    ${FONT_ROOT}/src/freetype/ft_truetype.c
    ${FONT_ROOT}/src/freetype/ft_ftsystem.c
    ${FONT_ROOT}/src/freetype/ft_raster.c
    ${FONT_ROOT}/src/freetype/ft_sfnt.c
    ${FONT_ROOT}/src/freetype/ft_autofit.c
    ${FONT_ROOT}/src/freetype/ft_ftbase.c
    ${FONT_ROOT}/src/freetype/ft_ftbitmap.c
    ${FONT_ROOT}/src/freetype/ft_ftdebug.c
    ${FONT_ROOT}/src/freetype/ft_ftglyph.c
    ${FONT_ROOT}/src/freetype/ft_ftgzip.c
    ${FONT_ROOT}/src/freetype/ft_ftinit.c
    ${FONT_ROOT}/src/freetype/ft_ftmm.c
  )
  target_include_directories(FontAdvanceDump PRIVATE
    ${FONT_ROOT}/include
    ${FONT_ROOT}/third_party/freetype/include
    ${FONT_ROOT}/third_party/stb
    ${REPO_ROOT}/lib/EpdFont
    ${REPO_ROOT}/lib/Utf8
    ${REPO_ROOT}/lib/MiniBidi
    ${REPO_ROOT}/test/font_cache_manager/vector_stubs
  )
  target_compile_definitions(FontAdvanceDump PRIVATE BOARD_HAS_PSRAM=1 FREEINK_FONT_ENABLE_AUTOHINT=1)
endif()
