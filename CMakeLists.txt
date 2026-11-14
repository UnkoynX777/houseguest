cmake_minimum_required(VERSION 3.25)
project(Houseguest VERSION 0.4.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HOUSEGUEST_BUILD_TESTS "Build unit tests" ON)
option(HOUSEGUEST_INSTANCES "Let HOUSEGUEST_INSTANCE rename the pipe and trace session (test builds only)" OFF)
option(HOUSEGUEST_SANITIZE "Build with AddressSanitizer" OFF)
option(HOUSEGUEST_FUZZ "Build the libFuzzer targets (implies HOUSEGUEST_SANITIZE)" OFF)

add_library(houseguest_warnings INTERFACE)
if(MSVC)
  target_compile_options(houseguest_warnings INTERFACE /W4 /WX /permissive- /GS /guard:cf /utf-8)
  target_link_options(houseguest_warnings INTERFACE /guard:cf /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA)
else()
  target_compile_options(houseguest_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)
endif()

if(HOUSEGUEST_FUZZ)
  set(HOUSEGUEST_SANITIZE ON)
endif()
if(HOUSEGUEST_SANITIZE AND MSVC)
  string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
  add_compile_definitions(_DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
  target_compile_options(houseguest_warnings INTERFACE /fsanitize=address /Zi)
  target_link_options(houseguest_warnings INTERFACE /INCREMENTAL:NO /DEBUG)
endif()

if(HOUSEGUEST_BUILD_TESTS)
  enable_testing()
endif()

configure_file(cmake/version.hpp.in ${CMAKE_BINARY_DIR}/generated/houseguest/version.hpp @ONLY)

add_subdirectory(core)

add_subdirectory(system)
add_subdirectory(service)
add_subdirectory(host)

if(HOUSEGUEST_FUZZ)
  add_subdirectory(fuzz)
endif()
