# dbengine

A from-scratch storage engine built while working through *Database
Internals* (Petrov, 2019) — Part I, chapters 1–7. Full chapter-by-chapter
design rationale, data structures, and API contracts live in
[`ARCHITECTURE.md`](./ARCHITECTURE.md).

## Status

Phase 1 complete: the in-memory `BPlusTreeEngine` supports split-propagating
inserts, lookups, scans, and non-rebalancing deletes; `SlottedPage` provides
the fixed binary page header, variable-size cell directory, cell codecs, and
CRC-32 verification needed before the tree becomes page-backed in Phase 2.

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
