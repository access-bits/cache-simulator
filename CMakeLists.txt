cmake_minimum_required(VERSION 3.20)
project(cachesim LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  # A simulator built without optimization is not a simulator you can run a
  # sweep with, so Release is the default rather than the empty build type
  # CMake would otherwise pick.
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CACHESIM_NATIVE_ARCH "Build for the host CPU (-march=native)" OFF)
option(CACHESIM_LTO "Link-time optimization in Release builds" ON)
option(CACHESIM_WERROR "Treat warnings as errors" OFF)
option(CACHESIM_BUILD_TESTS "Build the test suite" ON)
option(CACHESIM_BUILD_EXAMPLES "Build the examples" ON)
option(CACHESIM_BUILD_TOOLS "Build the trace tools" ON)
option(CACHESIM_BUILD_BENCH "Build the benchmarks" ON)

# ----------------------------------------------------------------- warnings
add_library(cachesim_flags INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cachesim_flags INTERFACE
      -Wall -Wextra -Wpedantic
      -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
      -Wcast-align -Wunused -Wdouble-promotion -Wformat=2)
  if(CACHESIM_WERROR)
    target_compile_options(cachesim_flags INTERFACE -Werror)
  endif()
  if(CACHESIM_NATIVE_ARCH)
    target_compile_options(cachesim_flags INTERFACE -march=native)
  endif()
endif()

# --------------------------------------------------------------- dependencies
# The only third-party code is one vendored header-only hash map. Nothing is
# fetched at configure time, so the build works with no network access.
add_library(unordered_dense INTERFACE)
target_include_directories(unordered_dense INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# zstd is optional: compressed traces are common enough to support, and rare
# enough that requiring the library would be rude.
find_package(PkgConfig QUIET)
set(CACHESIM_ZSTD_FOUND FALSE)
if(PkgConfig_FOUND)
  pkg_check_modules(ZSTD QUIET libzstd)
endif()
if(NOT ZSTD_FOUND)
  find_path(ZSTD_INCLUDE_DIR zstd.h)
  find_library(ZSTD_LIBRARY NAMES zstd)
  if(ZSTD_INCLUDE_DIR AND ZSTD_LIBRARY)
    set(ZSTD_FOUND TRUE)
    set(ZSTD_INCLUDE_DIRS ${ZSTD_INCLUDE_DIR})
    set(ZSTD_LIBRARIES ${ZSTD_LIBRARY})
  endif()
endif()

add_library(cachesim_zstd INTERFACE)
if(ZSTD_FOUND)
  set(CACHESIM_ZSTD_FOUND TRUE)
  target_compile_definitions(cachesim_zstd INTERFACE CACHESIM_HAVE_ZSTD=1)
  target_include_directories(cachesim_zstd INTERFACE ${ZSTD_INCLUDE_DIRS})
  target_link_libraries(cachesim_zstd INTERFACE ${ZSTD_LIBRARIES})
  if(ZSTD_LIBRARY_DIRS)
    target_link_directories(cachesim_zstd INTERFACE ${ZSTD_LIBRARY_DIRS})
  endif()
  message(STATUS "cachesim: zstd found; compressed traces (.zst) are supported")
else()
  target_compile_definitions(cachesim_zstd INTERFACE CACHESIM_HAVE_ZSTD=0)
  message(STATUS "cachesim: zstd NOT found; compressed traces will be rejected at runtime. "
                 "Install libzstd-dev (Debian/Ubuntu) or zstd (Homebrew) and re-configure.")
endif()

find_package(Threads REQUIRED)

# ------------------------------------------------------------------- library
add_library(cachesim_core STATIC
    src/internal/cache/cache.cpp
    src/internal/eviction/policy_registry.cpp
    src/internal/trace/byte_source.cpp
    src/internal/trace/record_layout.cpp
    src/internal/trace/trace_format.cpp
    src/internal/trace/trace_reader.cpp
)
target_include_directories(cachesim_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(cachesim_core
    PUBLIC unordered_dense Threads::Threads
    PRIVATE cachesim_flags
)
# zstd is PUBLIC because the CACHESIM_HAVE_ZSTD definition has to agree
# between this library and anything that includes its headers.
target_link_libraries(cachesim_core PUBLIC cachesim_zstd)

if(CACHESIM_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
  if(ipo_supported)
    set_property(TARGET cachesim_core PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  else()
    message(STATUS "cachesim: LTO unavailable (${ipo_error})")
  endif()
endif()

# --------------------------------------------------------------------- tools
if(CACHESIM_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

# ------------------------------------------------------------------ examples
if(CACHESIM_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# --------------------------------------------------------------------- tests
if(CACHESIM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
