# dbengine — Architecture & Build Plan

A from-scratch storage engine in C++20, built chapter by chapter through
*Database Internals* Part I (Petrov, 2019). This document is the complete
design reference: what each phase builds, why, the exact data structures
and APIs involved, how phases connect, and how to test each one before
moving on.

Chapters 1–4 map to Phases 0–2 (you've read these — this is the "now build
it" plan). Chapters 5–7 map to Phases 3–5 and are new material to read
phase-by-phase rather than all at once, for reasons explained in each
section.

---

## 0. Goals, scope, and non-goals

**Goal.** A pluggable key-value storage engine with two interchangeable
backends — a persistent B+Tree and an LSM tree — sitting behind one
`KVStore` interface, with real durability (WAL + crash recovery) and basic
concurrency control. Every concept implemented should trace back to a
specific section of the book.

**Non-goals.** No SQL layer, no query planner, no networking/wire protocol,
no distribution (that's Part II of the book, a separate project). The unit
of work throughout is a single-node, single-file storage engine. Anything
that would pull focus away from storage-engine internals is deliberately
left out.

**Success criteria per phase.** Each phase below ends with a "Definition of
done" checklist. Do not start the next phase until the current one's
checklist is fully green — the phases build on each other's on-disk
formats, and debugging a page-format bug three phases later, with WAL and
concurrency layered on top, is much harder than catching it now.

---

## 1. Language and tooling

- **Language:** C++20. Use `<span>`, structured bindings, and `concepts`
  where they clarify code; avoid reaching for template metaprogramming —
  this project's complexity should live in the algorithms, not the type
  system.
- **Build system:** CMake (already scaffolded).
- **Error handling:** no exceptions on the storage path. Every public
  method returns a `Status` (see `include/common/status.h`). This mirrors
  LevelDB/RocksDB/WiredTiger, all covered or referenced in the book, and it
  forces every call site to handle disk errors instead of letting them
  propagate silently — which matters a lot once WAL and recovery code needs
  to reason precisely about *what* failed and *when*.
- **Memory safety:** RAII everywhere (`std::unique_ptr` for owned pages,
  `std::fstream` wrapped in `DiskManager`, no raw `new`/`delete`). This is
  the main thing C++ buys you over C without giving up byte-level control
  of on-disk layout.
- **Testing:** hand-rolled assertions through Phase 1; introduce GoogleTest
  (via CMake `FetchContent`) at Phase 2, once split/merge edge cases need
  real parameterized tests and clearer failure output.
- **Compiler flags:** `-Wall -Wextra -Wpedantic` always on (already in
  `CMakeLists.txt`). Consider `-fsanitize=address,undefined` as a second
  CMake build type once Phase 3 introduces manual memory management for
  buffer pool frames — ASan will catch pin-count/use-after-free bugs far
  faster than staring at the code.

---

## 2. Repository layout

```
dbengine/
  CMakeLists.txt
  ARCHITECTURE.md          <- this file
  README.md
  include/
    common/
      config.h             <- PAGE_SIZE, page_id_t, etc.
      status.h             <- Status type
    storage/
      page.h                <- Page (Phase 0)
      disk_manager.h         <- DiskManager (Phase 0)
      slotted_page.h          <- (Phase 1)
      buffer_pool_manager.h    <- (Phase 3)
      log_manager.h            <- WAL writer (Phase 3)
      log_record.h             <- WAL record formats (Phase 3)
    index/
      kv_store.h            <- pluggable-engine interface (Phase 0)
      bplus_tree_engine.h    <- (Phase 0-2)
      bplus_tree_cow_engine.h <- (Phase 4, optional)
      lsm_engine.h             <- (Phase 5)
      sstable.h                <- (Phase 5)
      memtable.h               <- (Phase 5)
      bloom_filter.h           <- (Phase 5)
    txn/
      transaction.h           <- (Phase 3)
      lock_manager.h            <- (Phase 3)
      recovery_manager.h          <- (Phase 3)
  src/                      <- mirrors include/, one .cpp per header
  tests/                    <- one test file per module
  bench/                    <- benchmark harness (post-Phase 5)
  main.cpp
```

Each module directory maps to one box in the architecture diagram from
earlier in this conversation: `index/` holds the pluggable engines,
`storage/` holds the buffer pool + disk manager + WAL, and `txn/` holds the
concurrency/recovery code that coordinates across both.

---

## 3. The core interfaces (already scaffolded)

These three types are load-bearing across every phase — get them right now
because later phases build on top without revisiting them.

### `Status` (`common/status.h`)
Ok / NotFound / IOError / Corruption / InvalidArgument / NotImplemented.
Every fallible operation returns one.

### `Page` (`storage/page.h`)
```cpp
class Page {
  char data_[PAGE_SIZE];
  page_id_t page_id_;
  bool is_dirty_;
  int pin_count_;
  // Phase 3 adds: std::shared_mutex latch_;
};
```
Knows nothing about B+Tree or LSM structure — it's a dumb fixed-size byte
buffer with bookkeeping. Page *layout* (headers, cells) is a Phase 1
concern layered on top of `GetData()`.

### `KVStore` (`index/kv_store.h`)
```cpp
class KVStore {
 public:
  virtual Status Get(const std::string& key, std::string* value_out) = 0;
  virtual Status Put(const std::string& key, const std::string& value) = 0;
  virtual Status Delete(const std::string& key) = 0;
  virtual std::unique_ptr<Iterator> Scan(const std::string& start_key) = 0;
};
```
`BPlusTreeEngine`, `BPlusTreeCowEngine`, and `LsmEngine` all implement this.
A benchmark harness, a CLI, or (later, if you want it) an MCP server
wrapper should only ever hold a `KVStore*` — never a concrete engine type.
This is what makes "modular APIs" real instead of aspirational.

---

## 4. Phase 0 — Chapters 1 & 2: Foundations and in-memory B+Tree

**Book material:** Ch.1 (DBMS architecture, memory vs. disk-based, data
files vs. index files), Ch.2 (binary search trees, disk-based structures,
B-Tree basics).

**Why start here:** Chapter 2's split/merge algorithms are the trickiest
recursive logic in the whole project. Debugging them is far easier with
plain in-memory pointers than with page IDs, disk I/O, and serialization
all tangled in — so this phase deliberately ignores persistence.

### 4.1 Design decisions
- **Node fan-out:** pick a small order (e.g. 4-5 children) for early
  testing — small orders force splits/merges to trigger constantly with
  tiny datasets, which is exactly what you want for debugging. Increase to
  a realistic fan-out (order ~100+) once correctness is established and
  you're ready to reason about real page sizes in Phase 1.
- **Keys and values:** `std::string` for both, to keep Phase 0 decoupled
  from serialization concerns (Phase 1's problem).
- **Separation of concerns:** the tree logic (search/insert/split) is a
  pure, disk-agnostic algorithm operating over an abstract "node" concept.
  Phase 2 replaces the in-memory node representation with a page-backed one
  *without changing the algorithm's shape* — if you find yourself needing
  to touch the split/merge control flow in Phase 2, that's a signal Phase 0
  coupled algorithm to representation more than it should have.

### 4.2 Data structures
```cpp
struct LeafNode {
  std::vector<std::string> keys;
  std::vector<std::string> values;
  LeafNode* next = nullptr; // right-sibling pointer, book §"Ubiquitous B-Trees"
};

struct InternalNode {
  std::vector<std::string> keys;      // separator keys, size n
  std::vector<Node*> children;         // size n+1
};
```
Use a tagged union or a small class hierarchy (`Node` base with
`IsLeaf()`) — whichever you're more comfortable reasoning about recursively
is fine; this is one of the few places in the project where the "cleanest"
answer matters less than "the one you can debug fastest."

### 4.3 API surface
Implement directly on `BPlusTreeEngine` for now (no disk manager calls
yet — the field exists in the stub but stays unused until Phase 2):
- `Search(key) -> value or NotFound`
- `Insert(key, value)`: descend to the correct leaf, insert in sorted
  order, split on overflow, propagate a new separator key to the parent
  (recursively, up to a new root if the root itself splits).
- `Delete(key)`: descend, remove, and — for this phase — it's acceptable to
  *skip* rebalancing (merge/redistribute) on underflow and revisit it in
  Phase 2 once the on-disk cost of an underfull page is concrete and
  motivating. Note this simplification explicitly in a code comment so it
  doesn't get forgotten.

### 4.4 Testing
- Insert keys in ascending order, descending order, and random order;
  after each insert, walk the tree and assert it's still sorted and
  balanced (same leaf depth everywhere).
- Force splits at every level by choosing a small fan-out and inserting
  enough keys to split the root at least twice.
- Property-style test: insert N random keys, delete a random subset,
  verify `Search` matches an in-memory `std::map` used as an oracle.

### 4.5 Definition of done
- [x] Insert/Search work for at least 3 levels of tree depth.
- [x] Root-split (tree grows a new level) is tested explicitly.
- [x] An oracle test against `std::map` passes for 10k+ random operations.

---

## 5. Phase 1 — Chapter 3: Binary encoding and the page format

**Book material:** Ch.3 (binary encoding, page structure, slotted pages,
cell layout, variable-size data, versioning, checksumming).

**Why this phase exists on its own:** this is where the project stops
being "a tree in RAM" and starts being "a file format." Getting the page
layout right *before* wiring it into tree logic (Phase 2) means you can
unit-test serialization in isolation.

### 5.1 Page header (fixed layout, top of every page)

| Offset | Field | Size | Notes |
|---|---|---|---|
| 0 | `page_type` | 1 byte | leaf / internal / meta |
| 1 | `checksum` | 4 bytes | CRC32 over the rest of the page |
| 5 | `num_cells` | 2 bytes | slot count |
| 7 | `free_space_offset` | 2 bytes | where the next cell can be written |
| 9 | `right_sibling_page_id` | 4 bytes | leaf nodes only; `INVALID_PAGE_ID` otherwise |
| 13 | *(reserved)* | 3 bytes | pad to 16 for alignment |

This is a suggested layout, not gospel — the point is to fix *some*
explicit byte-for-byte header before writing serialization code, so
`Serialize`/`Deserialize` have a single source of truth to target.

### 5.2 Slotted page layout
Slots (an array of `(offset, length)` pairs) grow **down** from just after
the header; cell data grows **up** from the bottom of the page. Free space
is the gap in the middle. This is the layout the book uses precisely
because it lets you insert/delete variable-length records without shifting
every other record on the page — you only rewrite the slot array.

```cpp
struct Slot {
  uint16_t offset;
  uint16_t length;
};
```

### 5.3 Cell format
- **Leaf cell:** `[key_len: u16][key bytes][value_len: u16][value bytes]`
- **Internal cell:** `[key_len: u16][key bytes][child_page_id: i32]`

### 5.4 API surface (`storage/slotted_page.h`)
```cpp
class SlottedPage {
 public:
  explicit SlottedPage(Page* page); // wraps, does not own
  Status InsertCell(uint16_t slot_idx, std::span<const char> cell_data);
  Status DeleteCell(uint16_t slot_idx);
  std::span<const char> GetCell(uint16_t slot_idx) const;
  uint16_t NumCells() const;
  uint16_t FreeSpace() const;
  uint32_t ComputeChecksum() const;
  bool VerifyChecksum() const;
};
```

### 5.5 Testing
- Round-trip test: build a page, insert N cells of varying length, verify
  every cell reads back byte-identical.
- Fill-to-capacity test: insert cells until `InsertCell` correctly reports
  "page full" instead of corrupting adjacent cells.
- Checksum test: flip a random byte in a serialized page, assert
  `VerifyChecksum()` catches it.

### 5.6 Definition of done
- [x] Leaf and internal cell encode/decode round-trip exactly.
- [x] A full page correctly rejects further inserts instead of overflowing.
- [x] Checksum verification catches single-byte corruption reliably.

---

## 6. Phase 2 — Chapter 4: Persistent, on-disk B+Tree

**Book material:** Ch.4 (page header, binary search within a page,
propagating splits/merges, rebalancing, right-only appends, compression,
vacuum/maintenance).

This is where Phase 0's algorithm and Phase 1's page format merge: node
pointers become `page_id_t`, and every node access goes through
`DiskManager::ReadPage`/`WritePage` via `SlottedPage`.

### 6.1 Design decisions
- **Node addressing:** `InternalNode` children become `page_id_t` instead
  of `Node*`. Fetching a child means `DiskManager::ReadPage` + wrapping the
  buffer in a `SlottedPage`.
- **In-page search:** switch from linear scan to binary search over the
  slot array now that "read the whole page into a vector and scan" stops
  being free — the book's point in this chapter is that in-page search
  complexity actually matters once you're doing it on every disk-backed
  node access.
- **Free-list for deleted pages:** extend `DiskManager` with a simple
  free-list (a page of freed page-ids, or a linked list through freed
  pages themselves) so `Delete` + merge doesn't leak disk space forever —
  the naive Phase 0 `AllocatePage` bump-pointer strategy stops being
  acceptable once pages actually get freed.
- **Right-only append optimization:** if your workload is dominated by
  ascending-key inserts (common for auto-incrementing IDs), implement the
  book's optimization of skipping the "find split point" search when
  appending strictly to the rightmost leaf. Good candidate for a
  micro-benchmark showing the win.

### 6.2 API surface
`BPlusTreeEngine` now fully implements `KVStore` for real:
```cpp
Status Get(const std::string& key, std::string* value_out) override;
Status Put(const std::string& key, const std::string& value) override;
Status Delete(const std::string& key) override;
std::unique_ptr<Iterator> Scan(const std::string& start_key) override;
```
`Scan` walks leaf `right_sibling_page_id` pointers — this is exactly why
Phase 0's `LeafNode::next` and Phase 1's header field exist.

### 6.3 Testing
- Everything from Phase 0's test suite, rerun against the persistent
  engine.
- **Crash-consistency smoke test:** write N keys, kill the process (or
  just don't call a clean shutdown), reopen the file, verify all N keys are
  still readable. (Full crash *recovery* — mid-write torn pages — is Phase
  3's job; this test is just "did clean, completed writes survive.")
- Benchmark: measure `Put` throughput as tree size grows from 1k → 1M
  entries; this becomes the baseline you compare Phase 5's LSM engine
  against later.

### 6.4 Definition of done
- [x] All Phase 0 tests pass against the persistent engine.
- [x] Tree survives close/reopen with data intact.
- [x] Free-list reuses at least one deleted page in a targeted test.
- [x] `Scan` correctly returns a sorted range spanning multiple leaves.

---

## 7. Phase 3 — Chapter 5: Buffer pool, WAL, recovery, concurrency

**Book material:** Ch.5 (buffer management, recovery — analysis/redo/undo,
concurrency control). This is the largest phase in the project; budget
real time here.

### 7.1 Buffer pool manager
**Why it exists:** so far every `Get`/`Put` calls `DiskManager` directly —
every access hits disk. The buffer pool is an in-memory cache of `Page`
objects (a fixed-size pool of frames) sitting between the tree/LSM engines
and `DiskManager`.

```cpp
class BufferPoolManager {
 public:
  Page* FetchPage(page_id_t page_id);  // pins page, loads from disk if absent
  bool UnpinPage(page_id_t page_id, bool is_dirty);
  Status FlushPage(page_id_t page_id);
  Page* NewPage(page_id_t* page_id_out); // allocates + pins a fresh page
  Status FlushAllPages();

 private:
  std::vector<Page> pages_;                       // fixed-size frame pool
  std::unordered_map<page_id_t, frame_id_t> page_table_;
  std::vector<frame_id_t> free_list_;
  std::unique_ptr<Replacer> replacer_;             // eviction policy
  DiskManager* disk_manager_;
};
```
- **Eviction policy:** implement clock (a.k.a. second-chance) first — it's
  simpler than LRU to implement correctly and performs close to LRU in
  practice; the book discusses both. A page with `pin_count_ > 0` is never
  evictable — this invariant is the most common source of buffer-pool bugs,
  so assert it aggressively in debug builds.
- **From here on**, `BPlusTreeEngine` stops calling `DiskManager` directly
  and calls `BufferPoolManager::FetchPage`/`UnpinPage` instead. This is a
  meaningful refactor of Phase 2's code, not an addition — plan time for
  it.

### 7.2 Write-ahead log
```cpp
enum class LogRecordType { kInsert, kDelete, kUpdate, kBegin, kCommit, kAbort };

struct LogRecord {
  lsn_t lsn;
  txn_id_t txn_id;
  LogRecordType type;
  page_id_t page_id;
  // + before/after images or key/value payload, depending on type
};
```
Rule that makes recovery possible: **a page's changes must never hit disk
before the corresponding log record does** (write-ahead logging, the rule
the technique is named for). Enforce this by having `BufferPoolManager`
check the buffered log's flushed-LSN before flushing a dirty page.

### 7.3 Recovery (ARIES-lite)
Three passes, run on startup if the last shutdown wasn't clean:
1. **Analysis** — scan the log forward from the last checkpoint, rebuild
   the set of pages that were dirty and transactions that were active at
   crash time.
2. **Redo** — replay history forward from the earliest relevant LSN so the
   database reflects the exact state at crash time (including
   not-yet-committed changes — that's what undo is for).
3. **Undo** — roll back transactions that were active (uncommitted) at
   crash time, using the log's before-images.

### 7.4 Concurrency control
Start simple and be explicit that this is a deliberate scope choice:
- **Baseline:** page-level latches (`std::shared_mutex` on `Page`, already
  reserved for in section 3) — readers take shared latches, writers take
  exclusive ones, released as soon as a page access completes (not held for
  the whole transaction).
- **Stretch goal, not required:** basic MVCC (multi-version pages tagged
  with a transaction timestamp) if you want to explore it — but note this
  is a substantial addition and fine to skip; page latching alone is
  enough to make the buffer pool and recovery code correct under
  concurrent access.

### 7.5 Testing
- **Kill-and-recover test:** the centerpiece test of this phase. Write a
  batch of transactions, `SIGKILL` the process partway through (a test
  harness script, not a graceful shutdown), reopen, run recovery, verify:
  committed transactions are fully present, uncommitted ones are fully
  absent — no partial transaction state either way.
- Buffer pool: fill the pool past capacity, verify eviction picks an
  unpinned page and never touches a pinned one.
- Concurrency: spin up multiple threads doing concurrent `Put`s to
  different keys, verify no lost updates and no corrupted pages (run under
  `-fsanitize=thread` if available).

### 7.6 Definition of done
- [ ] Buffer pool correctly evicts under memory pressure, never evicting a
      pinned page.
- [ ] WAL record is durably flushed before its corresponding page.
- [ ] Kill-and-recover test passes: committed data survives, uncommitted
      data is rolled back.
- [ ] Concurrent `Put`s from multiple threads don't corrupt the tree
      (verified under a thread sanitizer if available).

---

## 8. Phase 4 — Chapter 6: A B-Tree variant (optional deep-dive)

**Book material:** Ch.6 (copy-on-write B-Trees, lazy B-Trees, FD-Trees,
Bw-Trees, cache-oblivious B-Trees).

**Scope note:** implement **one** variant; read about the rest without
building them. Bw-Trees and cache-oblivious trees are conceptually rich
but have a poor effort-to-learning ratio for a solo project at this point
— you'll get most of the insight from the book's explanation plus the
implementation experience you already have from Phases 0–3.

### 8.1 Recommended pick: copy-on-write B-Tree
**Why this one:** it pairs naturally with what you've already built (no
new page format needed) and it directly illustrates a trade-off you've
already felt the cost of — in-place updates (Phase 2/3's approach) require
careful latching to stay consistent under concurrent access; copy-on-write
sidesteps that by never mutating a page in place.

- On any modification, copy the target page (and every ancestor on the
  path to the root) to new page IDs instead of mutating in place, and swap
  in a new root pointer atomically at the end.
- No latch needed for read-only transactions — they simply pin whatever
  root was current when they started, which by construction can never be
  mutated underneath them.
- Trade-off to measure directly: write amplification (how many pages get
  copied per single-key update, roughly proportional to tree height) versus
  the concurrency-control simplicity gained.

### 8.2 Testing
- Snapshot isolation test: start a long-running `Scan`, mutate the tree
  concurrently, verify the scan sees a consistent snapshot from its start
  time.
- Write-amplification benchmark: measure pages-copied-per-update vs. tree
  height, compare against Phase 2/3's in-place engine.

### 8.3 Definition of done
- [ ] `BPlusTreeCowEngine` implements `KVStore` and passes the Phase 0/2
      correctness test suite.
- [ ] A concurrent reader observes a consistent snapshot while writes
      proceed.
- [ ] Write-amplification numbers are measured and written up.

---

## 9. Phase 5 — Chapter 7: Log-structured storage (LSM engine)

**Book material:** Ch.7 (LSM trees, compaction, merge-iterators, sorted
string tables, read/write/space amplification, concurrency in LSM trees).

This is the second `KVStore` implementation — and the payoff for having
built the interface cleanly back in Phase 0.

### 9.1 Components

```cpp
class MemTable {                 // in-memory, sorted
  // skip list or std::map; either is fine — the book uses skip lists for
  // O(log n) concurrent-friendly inserts, but a std::map is a reasonable
  // first cut while you get compaction and SSTable format right.
 public:
  void Put(const std::string& key, const std::string& value);
  void Delete(const std::string& key); // writes a tombstone, doesn't erase
  Status Get(const std::string& key, std::string* value_out);
  size_t ApproximateSizeBytes() const;
};

class SSTable {                  // immutable, on-disk, sorted
 public:
  static Status Build(const std::string& path, MemTable* source);
  Status Get(const std::string& key, std::string* value_out);
  std::unique_ptr<Iterator> NewIterator();
 private:
  BloomFilter bloom_;            // skip disk reads for definite non-members
  std::vector<IndexEntry> sparse_index_; // key -> block offset
};
```

### 9.2 Write path
1. `Put`/`Delete` go to the active `MemTable` (plus a WAL record first —
   reuse Phase 3's `LogManager`, this is the same durability mechanism,
   just protecting the memtable instead of buffer-pool pages).
2. When the memtable crosses a size threshold, freeze it (make it
   immutable) and flush it to a new `SSTable` file on disk. Writes continue
   into a fresh, empty memtable during the flush — the book's point about
   this being what makes LSM writes fast (sequential, append-only, no
   in-place page rewrites like the B+Tree needs).

### 9.3 Read path
`Get(key)`: check active memtable → immutable memtable(s) pending flush →
SSTables newest-to-oldest, until found or exhausted. This ordering matters:
the whole point is that newer data shadows older data, so you must stop at
the first match.

### 9.4 Compaction
Pick **one** strategy and justify the choice in a comment: size-tiered
(merge same-size-tier SSTables together, simpler, more read/space
amplification) or leveled (each level has a target size, more write
amplification but better read/space amplification). Size-tiered is the
easier starting point.

Implement the merge step as a k-way merge over SSTable iterators (the
book's approach: fill a priority queue with the first item from each
iterator, repeatedly pop the smallest, refill from that iterator, applying
tombstones as you go so deleted keys actually disappear during compaction
rather than living forever).

### 9.5 Bloom filters
```cpp
class BloomFilter {
 public:
  void Add(const std::string& key);
  bool MightContain(const std::string& key) const; // false positives OK,
                                                     // false negatives never
};
```
One per SSTable, checked before doing any disk I/O for that table on a
`Get` — this is the single highest-leverage optimization in the whole
phase, since it turns "check every SSTable" into "check only the ones that
might actually have the key."

### 9.6 Testing
- Oracle test again: N random `Put`/`Delete`/`Get` operations against
  `LsmEngine`, compared to `std::map`, across memtable flush and
  compaction boundaries.
- Tombstone test: delete a key, force a compaction that merges the
  tombstone with the original SSTable, verify the key is genuinely gone
  (not just shadowed) after compaction — a common correctness bug is a
  tombstone being dropped before it's had a chance to suppress the older
  value during merge.
- Bloom filter false-positive rate: measure empirically against the
  configured target rate.

### 9.7 Benchmark: B+Tree vs. LSM
This is the payoff moment — run the same workloads against both engines
through the shared `KVStore` interface and produce numbers for:
- Write-heavy (sequential and random key) throughput.
- Point-read latency, cold vs. warm cache.
- Range-scan throughput.
- Space amplification (on-disk size vs. logical data size) before and
  after compaction.
- Read amplification (disk reads per `Get`, with and without bloom
  filters).

These numbers turn chapter 7's "read/write/space amplification" discussion
from abstract trade-offs into measurements you produced yourself.

### 9.8 Definition of done
- [ ] `LsmEngine` implements `KVStore` and passes the oracle test suite.
- [ ] Memtable flush and at least one compaction round are exercised by
      tests, not just manually observed.
- [ ] Tombstones correctly suppress old values through a compaction.
- [ ] B+Tree vs. LSM benchmark numbers are produced and written up
      (`bench/results.md` is a reasonable place for this).

---

## 10. Cross-cutting concerns

### 10.1 Error handling philosophy (recap)
`Status` everywhere, no exceptions on the storage path. The one place
exceptions are acceptable: genuinely unrecoverable programmer errors
(e.g. a null `DiskManager*` passed to a constructor) — use `assert` for
those in debug builds, since they represent bugs, not runtime conditions
to handle gracefully.

### 10.2 Testing strategy summary
| Phase | Primary technique |
|---|---|
| 0 | Oracle test vs. `std::map`, structural invariant checks |
| 1 | Round-trip serialization tests, fuzz single-byte corruption |
| 2 | Phase 0 suite replayed on disk, close/reopen persistence test |
| 3 | Kill-and-recover test, concurrent-access stress test |
| 4 | Snapshot-isolation test, write-amplification benchmark |
| 5 | Oracle test across flush/compaction, tombstone-correctness test |

### 10.3 Suggested timeline
This is a learning project, not a sprint — but as a rough guide for
pacing: Phases 0–2 (chapters 1–4) are the fastest since you've already
read the material; Phase 3 (chapter 5) is the longest phase by a wide
margin and deserves the most calendar time; Phase 4 is optional and can be
skipped entirely without weakening the project; Phase 5 (chapter 7) is
comparable in size to Phase 2 once Phase 3's WAL is reusable.

---

## 11. What "done" looks like

A `KVStore*`-typed benchmark harness that can point at either
`BPlusTreeEngine` or `LsmEngine`, run identical workloads against both,
survive a kill-and-recover cycle on either, and produce the comparison
numbers described in section 9.7 — backed by a test suite that caught the
real bugs along the way, not just a demo that worked once.
