include(FetchContent)
set(JSON_Install OFF CACHE INTERNAL "")
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03 # v3.11.3
  GIT_SHALLOW OFF)
FetchContent_MakeAvailable(nlohmann_json)

add_library(houseguest_core STATIC
  src/glob.cpp
  src/protection.cpp
  src/evaluator.cpp
  src/analysis.cpp
  src/serialization.cpp)
add_library(houseguest::core ALIAS houseguest_core)
target_include_directories(houseguest_core PUBLIC include)
target_link_libraries(houseguest_core PUBLIC nlohmann_json::nlohmann_json PRIVATE houseguest_warnings)

if(HOUSEGUEST_BUILD_TESTS)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG fa43b77429ba76c462b1898d6cd2f2d7a9416b14 # v3.7.1
    GIT_SHALLOW OFF)
  FetchContent_MakeAvailable(Catch2)

  add_executable(houseguest_core_tests
    tests/glob_tests.cpp
    tests/evaluator_tests.cpp
    tests/analysis_tests.cpp
    tests/engine_tests.cpp
    tests/serialization_tests.cpp)
  target_link_libraries(houseguest_core_tests PRIVATE houseguest::core houseguest_warnings Catch2::Catch2WithMain)

  add_test(NAME houseguest_core_tests COMMAND houseguest_core_tests)
endif()
