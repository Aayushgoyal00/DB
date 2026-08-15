cmake_minimum_required(VERSION 3.16)
project(dbengine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Core library: everything that isn't the CLI entry point or tests lives here,
# so both the CLI and the test binaries link against the same code.
add_library(dbengine_core
  src/storage/disk_manager.cpp
  src/storage/slotted_page.cpp
  src/index/bplus_tree_engine.cpp
)
target_include_directories(dbengine_core PUBLIC include)

# CLI entry point (placeholder for now — Phase 0 only needs it to link).
add_executable(dbengine main.cpp)
target_link_libraries(dbengine PRIVATE dbengine_core)

# Diagnostic tool: read-only dump of the on-disk B+ tree structure.
add_executable(dump_tree tools/dump_tree.cpp)
target_link_libraries(dump_tree PRIVATE dbengine_core)

enable_testing()
add_subdirectory(tests)
