# silt — Architecture & Build Plan

A from-scratch disk-backed key-value storage engine in C++20, built chapter
by chapter through *Database Internals* Part I (Petrov, 2019). This
document is the complete design reference: what each phase builds, why,
the exact data structures and APIs involved, how phases connect, and how
to test each one before moving on.

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
silt/
  CMakeLists.txt
  ARCHITECTURE.md          <- this file
  README.md
  include/
    common/
      config.h             <- PAGE_SIZE, page_id_t, lsn_t, txn_id_t, etc.
      status.h             <- Status type
    storage/
      page.h                <- Page (Phase 0; latch + page_lsn added in 3.1/3.2)
      disk_manager.h         <- DiskManager (Phase 0)
      slotted_page.h          <- (Phase 1)
      replacer.h              <- Replacer + ClockReplacer (Phase 3.1)
      buffer_pool_manager.h    <- BufferPoolManager (Phase 3.1)
      log_manager.h            <- WAL writer (Phase 3.2)
      log_record.h             <- WAL record formats (Phase 3.2)
    index/
      kv_store.h            <- pluggable-engine interface (Phase 0)
      bplus_tree_engine.h    <- (Phase 0-2)
      bplus_tree_cow_engine.h <- (Phase 4, optional)
      lsm_engine.h             <- (Phase 5)
      sstable.h                <- (Phase 5)
      memtable.h               <- (Phase 5)
      bloom_filter.h           <- (Phase 5)
    txn/
      transaction.h           <- (Phase 3.4)
      lock_manager.h            <- (Phase 3.4)
      recovery_manager.h          <- ARIES-lite (Phase 3.2, engine hookup in 3.4)
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
  lsn_t page_lsn_;                // added Phase 3.2
  mutable std::shared_mutex latch_; // added Phase 3.1
};
```
Knows nothing about B+Tree or LSM structure — it's a dumb fixed-size byte
buffer with bookkeeping. Page *layout* (headers, cells) is a Phase 1
concern layered on top of `GetData()`. The latch and `page_lsn_` are the
bookkeeping the buffer pool and WAL need; they don't pull any
B+Tree/LSM concepts in.

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
**Status: implemented.**

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
  bool DeletePage(page_id_t page_id);

 private:
  size_t pool_size_;
  Page* pages_;                                     // fixed-size frame pool
  std::unordered_map<page_id_t, frame_id_t> page_table_;
  std::list<frame_id_t> free_list_;
  std::unique_ptr<Replacer> replacer_;              // eviction policy
  DiskManager* disk_manager_;
  LogManager* log_manager_;                         // optional; needed for WAL
  std::mutex latch_;
};
```
- **Eviction policy:** `ClockReplacer` (a.k.a. second-chance) is implemented
  in `storage/replacer.h`. Per-frame `in_use` + `ref_bit`; sweep hand gives
  recently-used frames a second chance. LRU is the gold standard but
  needs a doubly-linked list + hash, easy to get wrong under concurrency.
- **Pin invariant:** `pin_count_ > 0` → not evictable. Violating this
  gives use-after-free the moment eviction flushes the frame. Asserted
  in debug builds.
- **Per-page latch:** `Page::Latch()` returns a `std::shared_mutex`. The
  BPM's own `latch_` only protects *metadata* (page_table, free_list,
  replacer, pin_count, is_dirty). The page-bytes lock is separate so
  multiple threads can read different pages concurrently without
  serializing on the BPM.
- **Write-ahead rule hook:** every dirty-page writeback path
  (eviction, `FlushPage`, `FlushAllPages`) calls
  `log_manager_->Flush()` if `page.GetPageLSN() >
  log_manager_->GetFlushedLSN()`. This is the entire WAL correctness
  guarantee — see 7.2.
- **Refactor pending:** `BPlusTreeEngine` is still wired to
  `DiskManager` directly from Phase 2. Plumbing the BPM through the
  tree is the last step of this phase; until then the BPM exists and
  is unit-testable but the engine still bypasses it on disk access.

### 7.2 Write-ahead log
**Status: implemented (LogManager + LogRecord). Engine hookup is
Phase 3.4 work — the WAL is testable in isolation today.**

```cpp
enum class LogRecordType {
  kBegin, kCommit, kAbort, kInsert, kUpdate, kDelete, kCheckpoint
};

struct LogRecord {
  lsn_t lsn;           // assigned by LogManager at Append() time
  txn_id_t txn_id;
  LogRecordType type;
  page_id_t page_id;
  std::string key;            // empty for txn-control records
  std::string before_image;   // for undo
  std::string after_image;    // for redo
};
```

**On-disk framing** (one record on the wire):
```
+--------+-----+-----+----------+----------+
| u32    | u8  | u32 | i32      | var      |
| body_len type txn page_id   | payload   |
+--------+-----+-----+----------+----------+
| u32 CRC over (body_len + body)            |
+-------------------------------------------+
```

Length prefix lets us walk the file record-by-record. CRC catches
torn writes at the tail. LSN is assigned in memory (not in the
encoded body) so a recovery pass doesn't need to renumber anything.

**LogManager API** (`storage/log_manager.h`):
- `Append(r*)` — assigns `r->lsn`, writes to in-memory buffer.
- `Flush()` — fsyncs the buffer to disk, advances `flushed_lsn_`.
- `AppendAndFlush(r*)` — convenience for `COMMIT` records.
- `GetFlushedLSN()` — what the buffer pool asks to enforce write-ahead.
- `IterateAll(visitor)` — replays the log in order, stopping at the
  first torn-write / decode failure. Used by `RecoveryManager`.

**Write-ahead invariant (the rule that makes recovery possible):**
> A page's changes must never reach disk before the corresponding log
> record does.

Enforced in `BufferPoolManager` at every dirty-flush site:

```cpp
if (log_manager_ && p.GetPageLSN() > log_manager_->GetFlushedLSN()) {
  Status fs = log_manager_->Flush();   // durably advance the WAL
  if (!fs.ok()) return fs;
}
Status s = disk_manager_->WritePage(page_id, p.GetData());
```

This single check is what makes the crash-recovery theorem work: if
the WAL up to a page's LSN is on disk, the page is reproducible from
the log even if the page itself was never flushed.

### 7.3 Recovery (ARIES-lite)
**Status: implemented. `RecoveryManager` is a self-contained class in
`txn/`. It accepts any `RecoveryTarget` that exposes two methods
(`Redo(page, key, after)`, `Undo(page, key, before)`), so the engine
doesn't have to be ready to test recovery in isolation.**


Three passes, run on startup if the last shutdown wasn't clean:

1. **Analysis** — scan the log forward from offset 0; build:
   - `dirty_page_table_ : page_id → recLSN` (earliest log record that
     dirtied the page)
   - `active_txn_set_    : txn_id`
   - per-txn `txn_chains_` so UNDO can walk backward without
     re-scanning the file.
2. **Redo** — replay every record's after-image whose LSN is ≥ the
   smallest `recLSN` in the dirty-page table. Brings the engine to
   the exact state at crash time (including uncommitted work — the
   whole point is to handle crashes that happen *during* commit,
   where we can't tell whether the COMMIT record hit disk).
3. **Undo** — for every txn still in `active_txn_set_`, walk its
   chain backward applying before-images. Rolls back inflight work.

**Engine hookup (Phase 3.4):** `BPlusTreeEngine` will implement
`RecoveryTarget`. Redo re-inserts/applies the after-image on the
page (using a key → cell lookup); Undo reverts to the before-image.
Until then `RecoveryManager` is unit-tested with a `MockRecoveryTarget`
that records the call sequence and asserts ordering.

**Limitations / future work:**
- No checkpoint yet — every restart replays the full log. Fine for
  Phase 3; a real engine writes periodic checkpoints that truncate
  the prefix the analysis pass would have to scan.
- Full before/after images (not physiological logging). Uses more
  log bytes per update, but the algorithm is far easier to reason
  about and reason about correctly. The book discusses the
  trade-off in the recovery chapter.
- No "skip-if-alysn-applied" optimization on Redo. We trust that
  Redo's effect is idempotent at the engine layer (it is, for our
  slotted-page model where the same `Insert(key, value)` is a no-op
  if `key` already has that value).

### 7.4 Concurrency control
**Status: implemented — Two-Phase Locking at the key level, enforced
through `LockManager` in `txn/`.**

- **Baseline:** page-level latches (`std::shared_mutex` on `Page`,
  acquired inside every BPM-backed read/write) plus key-level
  Two-Phase Locking. `Transaction::Get/Put/Delete` acquire shared or
  exclusive locks through `LockManager` before they touch the tree;
  readers take shared latches on the page bytes, writers take exclusive
  ones. Locks are released by `Commit` or `Abort` (which also fires
  the `AbortRecord` / `CommitRecord` to the WAL).
- **`LockManager` design:** per-key `LockHead` (mutex + cv + FIFO
  request queue). Shared locks are granted to all consecutive shared
  requests at the head of the queue; exclusive locks wait for every
  earlier request. Upgrades (S→X) modify the existing queue entry in
  place rather than appending a second request, so they don't deadlock
  on their own shared lock.
- **Engine-level mutex (`BPlusTreeEngine::tree_latch_`)** protects
  structural changes that span multiple pages (root splits, page
  allocation, `StoreRootPageId`). Page-bytes work is per-page
  latched, so this only serializes "tree-wide" mutations.
- **Stretch goal (not implemented):** MVCC. Page latching + 2PL is
  enough to make the buffer pool and recovery code correct under
  concurrent access; MVCC is a substantially larger addition deferred
  to a future iteration.

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
- [x] Buffer pool correctly evicts under memory pressure, never evicting a
      pinned page.
- [x] WAL record is durably flushed before its corresponding page
      (write-ahead check lives in `BufferPoolManager`'s flush sites).
- [x] Kill-and-recover test passes: committed data survives, uncommitted
      data is rolled back, aborted data is rolled back. Tested both via
      a `MockRecoveryTarget` against `RecoveryManager` directly and at
      the engine level after the `BPlusTreeEngine` was rewired to the
      buffer pool.
- [x] Concurrent `Put`s from multiple threads don't corrupt the tree.
      Verified by the multi-threaded stress test in
      `tests/test_transaction_concurrency.cpp` (8 threads × 200 ops).

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

**Current status (where the project is right now):**
- ✅ Phase 0 — in-memory B+Tree
- ✅ Phase 1 — slotted page format with checksums
- ✅ Phase 2 — persistent, on-disk B+Tree
- ✅ Phase 3.1 — buffer pool manager (clock eviction, per-page latches)
- ✅ Phase 3.2 — write-ahead log + ARIES-lite recovery (`LogManager`,
  `LogRecord`, `RecoveryManager` built and unit-testable with a mock
  `RecoveryTarget`)
- ✅ Phase 3.3 — engine hookup: `BPlusTreeEngine` reworked to use
  `BufferPoolManager` for I/O, emit WAL records on every `Put`/`Delete`,
  and implement `RecoveryTarget`. This is the slice that turns the
  isolated modules into an actual crash-safe engine.
- ✅ Phase 3.4 — concurrency control via Two-Phase Locking
  (`LockManager` + `Transaction` in `txn/`) and multi-threaded stress
  tests under sanitizers.
- ⏳ Phase 4 — copy-on-write B-Tree variant
- ⏳ Phase 5 — LSM engine

---

## 11. What "done" looks like

A `KVStore*`-typed benchmark harness that can point at either
`BPlusTreeEngine` or `LsmEngine`, run identical workloads against both,
survive a kill-and-recover cycle on either, and produce the comparison
numbers described in section 9.7 — backed by a test suite that caught the
real bugs along the way, not just a demo that worked once.
