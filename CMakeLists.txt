cmake_minimum_required(VERSION 3.21)
project(zillions LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ZL_BUILD_APP "Build the game executable (needs a windowing system and OpenGL)" ON)
option(ZL_BUILD_TESTS "Build the headless test suite" ON)
option(ZL_NATIVE "Tune the build for the CPU of this machine" OFF)

# Flags shared by all first-party targets.
add_library(zl_options INTERFACE)
if(MSVC)
  target_compile_options(zl_options INTERFACE /W4 /permissive- /fp:fast)
else()
  target_compile_options(zl_options INTERFACE -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion)
  # The engine never relies on errno or exceptions from math functions.
  target_compile_options(zl_options INTERFACE -fno-math-errno)
  if(ZL_NATIVE)
    target_compile_options(zl_options INTERFACE -march=native)
  endif()
endif()

include(FetchContent)

# stb: single-header image writer (public domain).
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG f58f558c120e9b32c217290b80bad1a0729fbb2c)
FetchContent_MakeAvailable(stb)
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})

add_subdirectory(engine)
add_subdirectory(game)

if(ZL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
