cmake_minimum_required(VERSION 3.20)

project(stealpool
  VERSION 0.1.0
  DESCRIPTION "Work-stealing task scheduler"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(STEALPOOL_BUILD_TESTS "Build the GoogleTest suite" ON)
option(STEALPOOL_BUILD_EXAMPLES "Build the example programs" ON)
# Benchmarks only make sense optimised, so they default to ON for Release builds only.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(STEALPOOL_BENCHMARKS_DEFAULT ON)
else()
  set(STEALPOOL_BENCHMARKS_DEFAULT OFF)
endif()
option(STEALPOOL_BUILD_BENCHMARKS "Build the Google Benchmark suite" ${STEALPOOL_BENCHMARKS_DEFAULT})
set(STEALPOOL_SANITIZER "" CACHE STRING "Sanitizer for every target: empty, 'address' (ASan+UBSan) or 'thread'")
set_property(CACHE STEALPOOL_SANITIZER PROPERTY STRINGS "" address thread)

# Sanitizers must instrument every translation unit, dependencies included (a TSan build
# with uninstrumented GoogleTest reports false races), so they are applied globally.
if(STEALPOOL_SANITIZER STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
elseif(STEALPOOL_SANITIZER STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(NOT STEALPOOL_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unknown STEALPOOL_SANITIZER '${STEALPOOL_SANITIZER}'")
endif()

find_package(Threads REQUIRED)

# Warning flags for project targets only (third-party code is compiled with its own flags).
add_library(stealpool_warnings INTERFACE)
if(MSVC)
  target_compile_options(stealpool_warnings INTERFACE /W4)
else()
  target_compile_options(stealpool_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

add_library(stealpool
  src/shared_queue_pool.cpp
  src/thread_pool.cpp)
add_library(stealpool::stealpool ALIAS stealpool)
target_include_directories(stealpool PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(stealpool PUBLIC cxx_std_20)
target_link_libraries(stealpool
  PUBLIC Threads::Threads
  PRIVATE stealpool_warnings)

if(STEALPOOL_BUILD_TESTS)
  enable_testing()
endif()

if(STEALPOOL_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(STEALPOOL_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  # Newer Clang warns inside GoogleTest's own sources (char8_t printer). It is not our
  # code, so silence just that diagnostic for the gtest targets to keep builds clean.
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-Wno-character-conversion STEALPOOL_HAS_WNO_CHARACTER_CONVERSION)
  if(STEALPOOL_HAS_WNO_CHARACTER_CONVERSION)
    target_compile_options(gtest PRIVATE -Wno-character-conversion)
    target_compile_options(gtest_main PRIVATE -Wno-character-conversion)
  endif()
  add_subdirectory(tests)
endif()

if(STEALPOOL_BUILD_BENCHMARKS)
  include(FetchContent)
  FetchContent_Declare(googlebenchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.4.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googlebenchmark)
  add_subdirectory(bench)
endif()
