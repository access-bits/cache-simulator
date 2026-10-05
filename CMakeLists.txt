cmake_minimum_required(VERSION 3.20)
project(cachesim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(unordered_dense INTERFACE)
target_include_directories(unordered_dense INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

add_library(cachesim_core STATIC
    src/internal/cache/cache.cpp
    src/internal/cache/cache_structure.cpp
    src/policies/lru_policy.cpp
)
target_include_directories(cachesim_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(cachesim_core PUBLIC unordered_dense)

enable_testing()
add_subdirectory(tests)
