add_library(houseguest_system STATIC
  src/crypto.cpp
  src/text.cpp
  src/windows_info.cpp
  src/process.cpp
  src/accounts.cpp
  src/capabilities.cpp
  src/pipe_client.cpp)
add_library(houseguest::system ALIAS houseguest_system)
target_include_directories(houseguest_system PUBLIC include)
target_compile_definitions(houseguest_system PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(HOUSEGUEST_INSTANCES)
  target_compile_definitions(houseguest_system PUBLIC HOUSEGUEST_INSTANCES)
endif()
target_link_libraries(houseguest_system
  PUBLIC houseguest::core
  PRIVATE houseguest_warnings bcrypt crypt32 wintrust netapi32 wtsapi32 ole32 version shell32 shlwapi advapi32)

if(HOUSEGUEST_BUILD_TESTS)
  add_executable(houseguest_system_tests tests/system_tests.cpp)
  target_link_libraries(houseguest_system_tests PRIVATE houseguest::system houseguest_warnings Catch2::Catch2WithMain)
  add_test(NAME houseguest_system_tests COMMAND houseguest_system_tests)
endif()
