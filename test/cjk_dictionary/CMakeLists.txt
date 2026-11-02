add_executable(cjk_dictionary_test CjkDictionaryTest.cpp ${REPO_ROOT}/src/util/Dictionary.cpp)
target_include_directories(cjk_dictionary_test PRIVATE stubs ${REPO_ROOT}/src/util)
target_link_libraries(cjk_dictionary_test PRIVATE crosspoint_test_common)
add_test(NAME cjk_dictionary COMMAND cjk_dictionary_test)
