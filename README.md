# dbengine

A from-scratch storage engine built while working through *Database
Internals* (Petrov, 2019) — Part I, chapters 1–7. Full chapter-by-chapter
design rationale, data structures, and API contracts live in
[`ARCHITECTURE.md`](./ARCHITECTURE.md).

## Status

Phase 2 complete: `BPlusTreeEngine` is page-backed through `DiskManager` and
`SlottedPage`, with a persisted root metadata page, split propagation, leaf
range scans, clean close/reopen support, and reusable deallocated pages.

## Build

```
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make
ctest          # or: ./tests/test_disk_manager
./dbengine
```

Requires a C++20 compiler and CMake 3.16+.

## Layout

```
include/          public headers, one subdir per module
  common/          config constants, Status type
  storage/         Page, DiskManager
  index/           KVStore interface, engine headers
src/               implementation files, mirrors include/
tests/             hand-rolled tests for Phase 0-1; upgrade to GoogleTest
                   once assertions get numerous (see tests/CMakeLists.txt)
main.cpp           CLI entry point (placeholder)
```
