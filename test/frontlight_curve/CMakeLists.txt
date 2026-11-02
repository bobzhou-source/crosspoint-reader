add_executable(FrontlightCurveTest FrontlightCurveTest.cpp)
target_include_directories(FrontlightCurveTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../freeink-sdk/libs/hardware/FrontlightManager/include)
target_link_libraries(FrontlightCurveTest PRIVATE GTest::gtest_main)
gtest_discover_tests(FrontlightCurveTest)
