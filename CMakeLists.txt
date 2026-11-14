cmake_minimum_required(VERSION 3.25)
project(Houseguest VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HOUSEGUEST_BUILD_TESTS "Build unit tests" ON)

add_library(houseguest_warnings INTERFACE)
if(MSVC)
  target_compile_options(houseguest_warnings INTERFACE /W4 /WX /permissive- /GS /guard:cf /utf-8)
  target_link_options(houseguest_warnings INTERFACE /guard:cf /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA)
else()
  target_compile_options(houseguest_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)
endif()

if(HOUSEGUEST_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core)

add_subdirectory(system)
add_subdirectory(service)
add_subdirectory(host)
