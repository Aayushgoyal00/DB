# silt

A from-scratch key-value storage engine in C++, built by following
*Database Internals* (Petrov, 2019) chapter by chapter. Two interchangeable
backends — an on-disk B+Tree and (later) an LSM tree — sit behind one
interface, with real durability through a write-ahead log and ARIES-lite
recovery.

The point of the project is the internals: page layouts, slotted cells,
buffer pool eviction, write-ahead logging, redo/undo. Every concept maps
to a section of the book.

## What's in the box

A working storage engine with:

- **On-disk B+Tree** — page-backed nodes, slotted page format, page headers
  with CRC32 checksums, free-list for reused pages, range scans via
  right-sibling pointers.
- **Buffer pool** — fixed-size frame pool with a clock (second-chance)
  replacer, pin-count tracking, dirty-page writeback on eviction, and
  per-page reader/writer latches for concurrent access.
- **Write-ahead log** — append-only log of typed records (`Insert`,
  `Update`, `Delete`, `Begin`, `Commit`, `Abort`, `Checkpoint`) with
  per-record CRC, framed length prefixes, and torn-write detection.
- **ARIES-lite recovery** — three-pass recovery (analysis, redo, undo)
  on startup if the last shutdown wasn't clean. Committed work survives
  crashes; uncommitted work is rolled back.
- **Pluggable engine interface** — every backend implements the same
  `KVStore` (`Get`, `Put`, `Delete`, `Scan`). The B+Tree is wired up
  today; an LSM engine is on the roadmap and will drop in without
  touching call sites.

## Architecture

```
                 ┌─────────────────────┐
                 │   BPlusTreeEngine   │  ← pluggable KVStore impl
                 └──────────┬──────────┘
                            │
                 ┌──────────▼──────────┐
                 │  BufferPoolManager  │  ← frame pool + clock replacer
                 └──────────┬──────────┘
                            │
            ┌───────────────┼───────────────┐
            │               │               │
   ┌────────▼─────┐  ┌──────▼──────┐ ┌──────▼───────┐
   │  DiskManager │  │ LogManager │ │ SlottedPage  │
   │ (raw file I/O)│  │   (WAL)    │ │ (cell codec) │
   └──────────────┘  └─────────────┘ └──────────────┘

   On unclean shutdown → RecoveryManager runs:
     Analysis → Redo → Undo
```

Each layer has one job. The buffer pool is the only thing that calls
`DiskManager`. The WAL is enforced by the buffer pool: no dirty page
reaches disk before its log record does.

## Project layout

```
silt/
  include/
    common/        config, Status type
    storage/       Page, DiskManager, SlottedPage, Replacer,
                   BufferPoolManager, LogManager, LogRecord
    index/         KVStore interface, BPlusTreeEngine
    txn/           RecoveryManager (ARIES-lite)
  src/             mirrors include/, one .cpp per header
  tests/           one test binary per module
  tools/           dump_tree diagnostic
  main.cpp         CLI entry point
  CMakeLists.txt
```

## Build

Requirements: a C++20 compiler (GCC 12+, Clang 15+, MSVC 19.30+), CMake
3.16+, and Ninja (recommended).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Produces `build/silt` (CLI), `build/dump_tree` (diagnostic), and the
test binaries under `build/tests/`.

## Run

The CLI is a thin placeholder today. Real usage is through the test
binaries and by linking `dbengine_core` from your own code:

```cpp
#include "storage/disk_manager.h"
#include "storage/buffer_pool_manager.h"
#include "storage/log_manager.h"
#include "index/bplus_tree_engine.h"

dbengine::DiskManager dm("mydb.db");
dbengine::LogManager lm("mydb.wal");
dbengine::BufferPoolManager bpm(/*pool=*/1024, &dm, &lm);
dbengine::BPlusTreeEngine engine(&bpm);

engine.Put("hello", "world");
std::string v;
engine.Get("hello", &v);   // v == "world"
```

A diagnostic dump of an on-disk tree:

```bash
./build/dump_tree mydb.db
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

Or run an individual suite directly:

```bash
./build/tests/test_bplus_tree
./build/tests/test_slotted_page
```

## Roadmap

The plan is in `ARCHITECTURE.md`. Roughly:

- ✅ Phase 0 — in-memory B+Tree
- ✅ Phase 1 — slotted page format with checksums
- ✅ Phase 2 — persistent, on-disk B+Tree
- ✅ Phase 3 — buffer pool, WAL, ARIES-lite recovery
- ⏳ Phase 4 — copy-on-write B-Tree variant (snapshot isolation)
- ⏳ Phase 5 — LSM engine (MemTable, SSTable, compaction, bloom filters)

## References

- Alex Petrov, *Database Internals*, O'Reilly 2019
- ARIES paper (Mohan et al., 1992) — the recovery algorithm this
  project implements in simplified form
