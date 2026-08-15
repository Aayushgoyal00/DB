# dbengine

A small on-disk key-value database engine written in C++20. It follows the
Phase 0–2 approach described in *Database Internals* and stores data in a
page-backed B+ tree using a custom slotted-page format.

For the architecture notes and the deeper design rationale, see
[ARCHITECTURE.md](./ARCHITECTURE.md).

## Current status

This repository is in a working Phase 2 state.

What is already implemented:
- persistent metadata page at page 0
- on-disk root pointer tracking
- fixed-size page storage via `DiskManager`
- slotted pages with checksum verification
- B+ tree leaf and internal nodes
- key insert/update logic with split propagation
- leaf scan iteration across sorted keys
- simple CLI REPL for put/get/delete/scan/pages
- tree inspection tool to dump the real database structure from a file

What is not fully implemented yet:
- advanced delete rebalancing and page reclamation
- concurrency control
- WAL / crash recovery
- LSM backend
- SQL layer or client/server protocol

This is still a storage-engine prototype, not a production database.

## Quick start

### Prerequisites

- C++20 compiler
- CMake 3.16+
- Ninja or Make

### Linux / macOS

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/dbengine
```

If Ninja is not available:

```bash
cmake -S . -B build
cmake --build build
./build/dbengine
```

### Windows (MinGW / MSYS2 / Git Bash)

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/dbengine.exe
```

Or using a generator already available on the machine:

```bash
cmake -S . -B build
cmake --build build
./build/dbengine.exe
```

### Run the tests

```bash
ctest --test-dir build --output-on-failure
```

## CLI usage

Start the database:

```bash
./build/dbengine
```

Then use commands like:

```text
db> help
Commands:
  put <key> <value>        Insert or update a key
  get <key>                Read a key
  delete <key>             Remove a key
  scan <start_key> [limit] Scan keys in sorted order
  pages                    Show current allocated page count
  help                     Show this help
  exit                     Quit

DB> put user1 100
DB> get user1
100
DB> scan user1 10
DB> pages
DB> exit
```

## Inspect the current B+ tree

The repository includes a tree-dump utility that reads the real on-disk file and prints the structure of the root and child pages.

```bash
./build/dump_tree dbengine.db --pretty
```

This is useful for debugging page layout, split behavior, and root/leaf relationships without needing to write your own page parser.

## How the engine works today

The architecture is intentionally simple:

- `DiskManager` owns the database file and page allocation
- `SlottedPage` manages cell storage inside a page
- `BPlusTreeEngine` stores keys/values in leaf pages and separators in internal pages
- the root page ID is persisted in page 0 metadata
- inserts split a full leaf or internal node and propagate a separator upward

This means the project already behaves like a small, on-disk key-value database with B+ tree indexing, even though it is still a learning-oriented implementation rather than a polished production engine.

## Repository layout

```text
.
├── CMakeLists.txt
├── ARCHITECTURE.md
├── README.md
├── main.cpp
├── include/
│   ├── common/
│   ├── storage/
│   └── index/
├── src/
│   ├── storage/
│   └── index/
├── tests/
├── tools/
│   └── dump_tree.cpp
├── build/
├── dbengine.db
└── ...
```

## Recommended next steps

If you want to continue developing this engine, the natural follow-ups are:

1. implement delete rebalancing and page reuse
2. add proper duplicate-key semantics and validation rules
3. add WAL / crash recovery
4. support larger records and page occupancy tuning
5. benchmark insert/scan performance
6. add a stronger test suite for tree invariants

## Notes

This repository is aimed at learning and prototyping. It is already runnable and suitable for experimenting with B+ tree behavior, but it is not yet a full database system with transactional guarantees or production robustness.

