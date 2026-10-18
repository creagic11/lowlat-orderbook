cmake_minimum_required(VERSION 3.24)
project(lowlat_orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOB_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(LOB_TSAN     "Build with ThreadSanitizer" OFF)
option(LOB_NATIVE   "Tune for the build machine (-march=native); use for benchmarks" OFF)
option(LOB_BUILD_BENCH "Build Google Benchmark micro-benchmarks" ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O3 -g -DNDEBUG -fno-omit-frame-pointer")

if(LOB_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()
if(LOB_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()
if(LOB_NATIVE)
  add_compile_options(-march=native)
endif()

# ---------------------------------------------------------------- dependencies
include(FetchContent)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googletest)

if(LOB_BUILD_BENCH)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.4
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(benchmark)
endif()

# ---------------------------------------------------------------- library
# Header-only core: decoder, books, matching engine, rings.
add_library(lob INTERFACE)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------------------------------------------------------- executables
file(GLOB LOB_APPS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*_main.cpp)
foreach(app_src ${LOB_APPS})
  get_filename_component(app_name ${app_src} NAME_WE)
  string(REPLACE "_main" "" app_name ${app_name})
  add_executable(${app_name} ${app_src})
  target_link_libraries(${app_name} PRIVATE lob)
endforeach()

# ---------------------------------------------------------------- tests
enable_testing()
include(GoogleTest)
file(GLOB LOB_TESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp)
add_executable(lob_tests ${LOB_TESTS})
target_link_libraries(lob_tests PRIVATE lob GTest::gtest_main)
target_compile_definitions(lob_tests PRIVATE LOB_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
gtest_discover_tests(lob_tests DISCOVERY_TIMEOUT 60)

# ---------------------------------------------------------------- benchmarks
if(LOB_BUILD_BENCH)
  file(GLOB LOB_BENCHES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bench/*_bench.cpp)
  if(LOB_BENCHES)
    add_executable(lob_bench ${LOB_BENCHES})
    target_link_libraries(lob_bench PRIVATE lob benchmark::benchmark_main)
  endif()
endif()
