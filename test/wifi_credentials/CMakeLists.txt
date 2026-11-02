add_executable(WifiCredentialTest
  WifiCredentialTest.cpp
  ../../src/WifiCredentialStore.cpp
  ../../src/network/WifiConnection.cpp
  ../../lib/Serialization/PersistableStore.cpp
  ../../lib/Serialization/ObfuscationUtils.cpp
)
target_include_directories(WifiCredentialTest PRIVATE
  stubs ../../src ../../lib/Serialization
  ../../.pio/libdeps/x4pro-gh_release/ArduinoJson/src
)
target_link_libraries(WifiCredentialTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(WifiCredentialTest)
