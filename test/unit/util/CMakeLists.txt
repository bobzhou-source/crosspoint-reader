add_executable(ConceptEngineTest ConceptEngineTest.cpp ${REPO_ROOT}/src/util/ConceptEngine.cpp)
target_include_directories(ConceptEngineTest PRIVATE stubs ${REPO_ROOT}/src)
target_link_libraries(ConceptEngineTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ConceptEngineTest)
