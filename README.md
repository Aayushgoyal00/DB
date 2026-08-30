# silt — C++20 Key-Value Storage Engine

[![Language](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Build System](https://img.shields.io/badge/Build-CMake%20%2B%20Ninja-orange.svg)](https://cmake.org/)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)

**silt** is a disk-backed, embeddable key-value storage engine implemented from first principles in C++20, based on modern database architecture patterns (*Database Internals*, Petrov 2019; ARIES, Mohan et al. 1992).

It provides durable, ordered key-value storage behind a pluggable `KVStore` interface, featuring slotted-page storage management, an in-memory buffer pool with clock-sweep page replacement, append-only write-ahead logging (WAL), and automated crash recovery.

---

## 🏛️ Architecture & System Design

The engine is built around strict separation of concerns across four core subsystems:

```
                      ┌─────────────────────────────────────────┐
                      │             KVStore Interface           │
                      └────────────────────┬────────────────────┘
                                           │
                      ┌────────────────────▼────────────────────┐
                      │         BPlusTreeEngine (Index)         │
                      └────────────────────┬────────────────────┘
                                           │
                      ┌────────────────────▼────────────────────┐
                      │        BufferPoolManager (Cache)        │
                      │   ┌─────────────────────────────────┐   │
                      │   │ ClockReplacer (Second-Chance)   │   │
                      │   └─────────────────────────────────┘   │
                      └───────┬─────────────────────────┬───────┘
                              │                         │
            (Write-Ahead Rule)│                         │ (Page I/O)
                              ▼                         ▼
                 ┌────────────────────────┐ ┌───────────────────────┐
                 │   LogManager (WAL)     │ │     DiskManager       │
                 │ ┌────────────────────┐ │ │ ┌───────────────────┐ │
                 │ │ CRC32 Framed Logs  │ │ │ │ 4KB Page Storage  │ │
                 │ └────────────────────┘ │ │ └───────────────────┘ │
                 └───────────┬────────────┘ └───────────────────────┘
                             │
                             ▼ (On Unclean Startup)
                 ┌────────────────────────┐
                 │    RecoveryManager     │
                 │  Analysis ➔ Redo ➔ Undo │
                 └────────────────────────┘
```

---

## ✨ Core Capabilities & Subsystems

### 1. Slotted-Page Storage & Binary Serialization
* **4KB Fixed Page Layout:** Aligns with OS block and sector boundaries to minimize write amplification and partial page writes.
* **Dual-Direction Allocation:** Variable-length keys and values grow upward from the bottom of the page, while the slot directory (`offset`, `length` pairs) grows downward from the header. This enables in-place record deletions and updates without shifting neighboring payload data.
* **Integrity Verification:** Every serialized page header carries a CRC32 checksum computed across the slot table and cell data, detecting silent data corruption and bit rot on read.

### 2. Buffer Pool Management & In-Memory Caching
* **Fixed-Size Frame Cache:** Mediates all disk access between the engine and physical files, preventing OS page-cache thrashing.
* **Clock (Second-Chance) Eviction:** Approximates LRU cache eviction with $O(1)$ amortized overhead using an eviction sweep hand and per-frame access reference bits.
* **Pin-Count Lifetime Tracking:** Frames actively accessed by queries maintain non-zero pin counts, enforcing the invariant that active execution paths are never invalidated by eviction sweeps.
* **Per-Page Latches:** Each `Page` encapsulates an in-memory `std::shared_mutex`, enabling concurrent multi-reader access and exclusive writer synchronization.

### 3. Durability, WAL & ARIES-lite Recovery
* **Strict Write-Ahead Invariant:** Dirty pages maintain a page-level log sequence number (`page_lsn_`). The buffer pool guarantees that a dirty frame is never flushed to disk until the WAL has been flushed up to that page's LSN.
* **Framed Log Records:** WAL records contain length prefixes, monotonic 64-bit LSNs, transaction identifiers, before/after images for all state modifications, and end-of-frame CRC checksums for torn-write detection.
* **3-Pass Crash Recovery:**
  1. **Analysis Pass:** Scans the log forward to reconstruct active transactions and the Dirty Page Table (DPT).
  2. **Redo Pass:** Replays history forward from the smallest un-flushed `recLSN`, restoring the engine to the exact state at the time of failure.
  3. **Undo Pass:** Scans backward through uncommitted transactions, applying before-images to roll back inflight operations.

### 4. Disk-Backed B+Tree Engine
* **Pluggable Interface:** Implements `KVStore` (`Get`, `Put`, `Delete`, `Scan`).
* **Node Splitting & Free-List Recycling:** Dynamically balances leaf and internal nodes on overflow, cascading separator keys up to a new root. Deallocated pages are tracked via an in-file free list to prevent disk space leaks.
* **Sequential Leaf Sibling Pointers:** Leaf nodes maintain right-sibling page references, enabling $O(\log N + K)$ range scans without re-traversing internal index levels.

---

## 🛠️ Tech Stack & Engineering Standards

* **Language:** C++20 (`std::span`, structured bindings, `std::shared_mutex`, RAII).
* **Error Handling:** LevelDB/RocksDB-style `Status` returns across all storage paths — strict avoidance of exceptions in I/O and traversal hot paths ensures precise error handling and crash resilience.
* **Build System:** CMake (3.16+) + Ninja.
* **Compiler Flags:** Clean builds with `-Wall -Wextra -Wpedantic`.

---

## 📁 Repository Structure

```
silt/
  include/
    common/        config.h (PAGE_SIZE, types), status.h (Status result type)
    storage/       page.h, disk_manager.h, slotted_page.h, replacer.h,
                   buffer_pool_manager.h, log_manager.h, log_record.h
    index/         kv_store.h (interface), bplus_tree_engine.h
    txn/           recovery_manager.h (ARIES-lite)
  src/             Implementation files mirroring include/ structure
  tests/           Modular test suites (disk manager, slotted page, B+Tree)
  tools/           dump_tree.cpp (On-disk inspection diagnostic)
  main.cpp         CLI driver
  CMakeLists.txt   Project build definitions
```

---

## 🚀 Quick Start

### Prerequisites
* A C++20 compliant compiler (GCC 12+, Clang 15+, or MSVC 2022)
* CMake 3.16+
* Ninja build system (recommended)

### Building the Project

```bash
# Clone the repository
git clone https://github.com/Aayushgoyal00/dbengine.git
cd dbengine

# Configure and compile with Ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This will build:
* `libdbengine_core.a` — Core engine static library
* `dbengine` — CLI binary
* `dump_tree` — On-disk inspection and diagnostic tool
* Unit & property test suites under `build/tests/`

---

## 💻 Usage Example

```cpp
#include <iostream>
#include <string>

#include "storage/disk_manager.h"
#include "storage/log_manager.h"
#include "storage/buffer_pool_manager.h"
#include "index/bplus_tree_engine.h"

int main() {
    using namespace dbengine;

    // 1. Initialize disk manager and WAL
    DiskManager disk_manager("data.db");
    LogManager log_manager("data.wal");

    // 2. Instantiate buffer pool with 1,024 frames (4MB cache)
    BufferPoolManager bpm(1024, &disk_manager, &log_manager);

    // 3. Mount the B+Tree engine
    BPlusTreeEngine db(&bpm);

    // 4. Perform Key-Value Operations
    db.Put("user:1001", "Alice");
    db.Put("user:1002", "Bob");

    std::string value;
    Status s = db.Get("user:1001", &value);
    if (s.ok()) {
        std::cout << "Found: " << value << "\n"; // Outputs: Alice
    }

    // 5. Ordered Range Scan
    auto it = db.Scan("user:1000");
    while (it->Valid()) {
        std::cout << it->Key() << " => " << it->Value() << "\n";
        it->Next();
    }

    return 0;
}
```

---

## 🧪 Testing & Verification

The test suite validates structural correctness, edge-case splits, checksum integrity, and persistence guarantees:

```bash
# Run all tests via CTest
ctest --test-dir build --output-on-failure
```

Or run individual test suites directly:

```bash
# Test slotted page serialization, cell packing, and CRC corruption detection
./build/tests/test_slotted_page

# Test disk allocator, page reads/writes, and file growth
./build/tests/test_disk_manager

# Test B+Tree balanced tree depth, splits, and range scans
./build/tests/test_bplus_tree
```

### Inspecting On-Disk Structures
Use the `dump_tree` diagnostic utility to inspect physical page layouts, slot allocations, and node links directly from a database file:

```bash
./build/dump_tree data.db
```

---

## 🗺️ Roadmap & Future Work

- [x] **Phase 0:** In-Memory B+Tree baseline & oracle property tests.
- [x] **Phase 1:** Slotted-page binary encoder with CRC32 integrity verification.
- [x] **Phase 2:** Disk-backed B+Tree engine with free-list space reclamation.
- [x] **Phase 3.1:** Buffer pool manager with clock-sweep eviction and per-page latches.
- [x] **Phase 3.2:** Write-Ahead Logging (WAL) with frame checksums and 3-pass ARIES-lite recovery.
- [ ] **Phase 4:** Copy-On-Write (COW) B-Tree backend for latch-free snapshot isolation.
- [ ] **Phase 5:** Log-Structured Merge (LSM) Tree engine with MemTable, SSTables, Bloom filters, and leveled compaction.

---

## 📚 References
* **Alex Petrov**, *Database Internals: A Deep Dive into How Distributed Data Systems Work*, O'Reilly Media (2019).
* **C. Mohan, Don Haderle, et al.**, *ARIES: A Transaction Recovery Method Supporting Fine-Granularity Locking and Partial Rollbacks Using Write-Ahead Logging*, ACM TODS (1992).
