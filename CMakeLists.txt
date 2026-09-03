cmake_minimum_required(VERSION 3.16)
project(dbengine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Core library: everything that isn't the CLI entry point or tests lives here,
# so both the CLI and the test binaries link against the same code.
add_library(dbengine_core
  src/storage/disk_manager.cpp
  src/storage/slotted_page.cpp
  src/storage/replacer.cpp
  src/storage/buffer_pool_manager.cpp
  src/storage/log_record.cpp
  src/storage/log_manager.cpp
  src/txn/lock_manager.cpp
  src/txn/recovery_manager.cpp
  src/index/bplus_tree_engine.cpp
)
target_include_directories(dbengine_core PUBLIC include)

# CLI entry point (placeholder for now — Phase 0 only needs it to link).
add_executable(dbengine main.cpp)
target_link_libraries(dbengine PRIVATE dbengine_core)

# Diagnostic tool: read-only dump of the on-disk B+ tree structure.
add_executable(dump_tree tools/dump_tree.cpp)
target_link_libraries(dump_tree PRIVATE dbengine_core)

# Standalone random-KV benchmark that creates a DB + WAL and leaves both files on disk.
add_executable(random_kv_benchmark tools/random_kv_benchmark.cpp)
target_link_libraries(random_kv_benchmark PRIVATE dbengine_core)

# Inspect generated DB and WAL files in a readable way.
add_executable(inspect_demo_files tools/inspect_demo_files.cpp)

enable_testing()
add_subdirectory(tests)
