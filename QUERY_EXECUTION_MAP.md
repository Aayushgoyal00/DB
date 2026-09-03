# Query Execution Map: From API to Disk

This document maps a single database query through every abstraction layer in this storage engine and records the decisions each layer must make before the work is actually executed.

The engine supports four core query families:

- Insert / Put
- Get / Read
- Delete / Remove
- Scan / Range Read (included as a related read path)

The emphasis here is not just on the final result, but on the full decision chain: what is checked, what is chosen, what is protected, what gets logged, and what gets persisted.

---

## 1. Top-level abstraction ladder

```text
Application / Caller
        |
        v
KVStore interface
        |
        v
BPlusTreeEngine
        |
        +--> Transaction layer (optional)
        |          |
        |          +--> LockManager (2PL)
        |          |
        |          +--> Transaction metadata / txn_id / before-image
        |
        +--> Tree logic layer
        |          |
        |          +--> Find leaf / root search / split / merge / rebalancing
        |          +--> Internal/Leaf page logic
        |
        +--> Storage access layer
                   |
                   +--> BufferPoolManager
                   |          |
                   |          +--> Page cache / frame table / pinning / eviction
                   |
                   +--> LogManager / WAL
                   |          |
                   |          +--> Log record creation / flush ordering / durability
                   |
                   +--> DiskManager
                              |
                              +--> File allocation / page reads / page writes / CRC / file growth
```

This is the real query path in this project. A query is never just “read a key” or “write a key”; it crosses at least five layers of semantics before the bytes hit disk.

---

## 2. Core decision model by layer

Each layer answers a different kind of question.

| Layer | Main question | Important decisions | Typical output |
|---|---|---|---|
| Application | What operation is requested? | Insert, Get, Delete, Scan, explicit txn or auto txn | Query intent |
| KVStore API | Which engine interface is used? | `Get`, `Put`, `Delete`, `Scan` contract | Request enters engine |
| Engine logic | Which tree path matches the key? | root page, leaf page, split/merge, key existence | page IDs, node mutation plan |
| Transaction layer | Is concurrency control needed? | txn vs autocommit, shared/exclusive lock, before-image | lock grant or reject |
| Buffer pool | Is page already cached? | fetch vs allocate vs evict, pin/unpin, mark dirty | pointer to in-memory page |
| WAL layer | Is durability required before commit? | log format, LSN, flush ordering, before/after image | WAL record + flush |
| Page serialization | How is key/value encoded on page? | slotted page layout, offset/length pairs, CRC | serialized page bytes |
| Disk layer | Where does data live on disk? | page ID, file offset, growth, file integrity | actual bytes on disk |
| Recovery layer | What if crash happens mid-write? | analyze/redo/undo decision | consistent state after restart |

---

## 3. Common query lifecycle

Every request passes through this common lifecycle:

1. Request arrives at the public API.
2. Engine decides whether this is transactional or simple direct operation.
3. If it is a write, it decides whether to capture a before-image and generate WAL.
4. It locates the correct page in the B+Tree.
5. It either reads the key/value or mutates the leaf/internal node.
6. It writes the affected page to the buffer pool and marks it dirty if needed.
7. It flushes WAL-first ordering for durability if required.
8. It unpins the page and returns status to caller.
9. If crash occurs, recovery decides whether to redo or undo.

---

## 4. Query map for INSERT / PUT

### 4.1 Top-level flow

```text
Client Put(key, value)
        |
        v
Is this a transactional Put(txn, key, value)?
        |
        +--> Yes --> Acquire exclusive lock on key
        |                |
        |                +--> if locked by another txn: wait / block
        |
        +--> No  --> Use autocommit semantics

        v
Prepare write path
        |
        +--> Validate key/value constraints
        +--> Determine whether key already exists
        +--> Decide if this is a leaf update or an insert into a new key slot
        |
        v
Find target leaf page
        |
        +--> Root -> internal nodes -> leaf
        +--> if key exists: overwrite value in existing tuple
        +--> if key absent: append new key/value into leaf slot array

        v
Does leaf overflow?
        |
        +--> No --> serialize updated leaf and return
        |
        +--> Yes --> split leaf
                    |
                    +--> redistribute entries between left and right leaf
                    +--> create separator key for parent
                    +--> propagate split upward if needed
                    +--> possibly create new root

        v
Write-ahead logging decision
        |
        +--> If WAL-backed engine: create record with before/after image
        +--> flush WAL to durable storage before page flush

        v
Buffer pool decision
        |
        +--> fetch page from cache or disk
        +--> pin page
        +--> mark page dirty
        +--> unpin after write

        v
Disk write / commit
        |
        +--> page content serialized to slotted page format
        +--> CRC / checksum applied
        +--> page persisted to disk
        +--> commit returns success
```

### 4.2 Decisions made at each abstraction layer

#### Application layer
- Is the caller using the public `KVStore` interface or transactional API?
- Is the key valid for this engine?
- Should this be treated as an overwrite or a fresh insert?

#### Engine layer
- Is the root initialized?
- Is the key already present in the tree?
- Does the leaf page have capacity to store the new record?
- Does the tree need to split one level or multiple levels?
- Should parent separator keys be updated?
- Does a new root need to be created?

#### Transaction layer
- Was a transaction provided?
- Does the transaction already hold a lock for this key?
- Does the lock mode need to be exclusive?
- Should the operation be buffered until commit? (in a real transactional engine, this is a design choice)
- Is an undo image needed for abort?

#### Buffer pool layer
- Is the page already in the page table?
- Is the page currently pinned and therefore not evictable?
- Is this a dirty write requiring later flush?
- Should the page be loaded from disk or remain in memory?
- Should the eviction policy pick a victim before the new page is loaded?

#### WAL layer
- Is recovery enabled?
- Should this be logged as a single mutation or a set of sub-steps?
- Should the record include before-image and after-image?
- Has the WAL been flushed before the page is durable?
- If a crash occurs before flush, should the operation be considered uncommitted?

#### Disk / page layer
- Is the page a leaf or internal page?
- How do we encode key/value into slot layout?
- Where do we place the cell data and slot directory?
- Is the checksum valid after writing?
- Can the page be safely updated in place without corrupting neighboring entries?

#### Recovery layer
- If process crashes after log flush but before page flush, does redo recover the post-image?
- If the process crashes before commit, does undo restore the previous state?
- Is the transaction still active or marked committed?

---

## 5. Query map for GET / READ

### 5.1 Top-level flow

```text
Client Get(key)
        |
        v
Is this a transactional Get(txn, key)?
        |
        +--> Yes --> Acquire shared lock on key
        |                |
        |                +--> if write lock exists by another txn: wait
        |
        +--> No --> Use autocommit read path

        v
Locate leaf page
        |
        +--> traverse root -> internal -> leaf

        v
Does key exist in leaf?
        |
        +--> No --> return NotFound
        |
        +--> Yes --> read value

        v
Buffer pool decision
        |
        +--> page in memory? fetch from cache
        +--> page on disk? read from DiskManager
        +--> pin page while reading
        +--> unpin after access

        v
Return value
        |
        +--> if transaction is active, keep read consistent
        +--> return Status::OK with value
```

### 5.2 Decisions made at each abstraction layer

#### API layer
- Is the caller asking for a value by exact key or a range?
- Is the key empty or invalid?
- Should a missing value return `NotFound` or a low-level I/O error?

#### Search layer
- Which root page is active?
- Which child branch should be followed?
- Is the key in this internal separator region or in a leaf?
- Does the leaf page have the required key?

#### Concurrency layer
- Can multiple readers read the same key concurrently?
- Should the read block behind an exclusive writer?
- Does the transaction require snapshot isolation or current read semantics?

#### Page cache layer
- Has the page been loaded before and remained cached?
- Is this a cache hit or miss?
- Should the page be pinned while reading to avoid eviction mid-read?

#### Serialization layer
- Is the page fully present in memory?
- Is the slotted page structure valid?
- Is the checksum or slot metadata consistent?
- Does the page contain the requested key and value bytes?

#### Result layer
- Return the string value.
- Or return a failure status if missing or corrupted.

---

## 6. Query map for DELETE / REMOVE

### 6.1 Top-level flow

```text
Client Delete(key)
        |
        v
Transactional or autocommit?
        |
        +--> Yes --> Acquire exclusive lock on key
        |                |
        |                +--> if another txn holds read or write lock: wait
        |
        +--> No --> Direct delete path

        v
Locate key in tree
        |
        +--> traverse to leaf

        v
Does key exist?
        |
        +--> No --> return NotFound
        |
        +--> Yes --> create before-image for undo or rollback

        v
Write-ahead logging decision
        |
        +--> generate delete log record
        +--> include before-image and delete marker if needed
        +--> flush WAL before page flush

        v
Mutate leaf page
        |
        +--> remove slot from leaf
        +--> compact slot array / free space
        +--> update sibling pointers if needed
        +--> maintain sorted order

        v
Does tree require underflow handling or rebalance?
        |
        +--> No -> continue
        |
        +--> Yes -> rebalance or merge nodes, update parent separators

        v
Buffer pool decision
        |
        +--> fetch page and pin it
        +--> mark dirty
        +--> unpin after write

        v
Commit or abort
        |
        +--> commit finalizes write
        +--> abort may restore original value via before-image
```

### 6.2 Decisions made at each abstraction layer

#### API layer
- Is deletion allowed to be a no-op if key is absent?
- Should missing rows return `NotFound` or `OK`?
- Is this a logical delete or a physical delete from the page?

#### Tree layer
- Is the key present in the current leaf?
- Does deleting this key cause underflow?
- Should the engine rotate or merge neighboring nodes?
- Does the parent separator need to be adjusted?
- Does the root become empty or need replacement?

#### Concurrency layer
- Is exclusive write access required for the record?
- If another transaction is scanning or reading the same key, should delete block?
- Should locks be released at commit or after each mutation?

#### Logging layer
- What is the before-image of the record?
- Should the log represent a delete operation or a value replacement?
- How far back must recovery go to avoid inconsistent state?

#### Page serialization layer
- Does removing the record leave invalid slots or stale bytes?
- Is the slot array still in ascending order?
- Does the page checksum need to be recomputed?

#### Recovery layer
- If crash happens before page flush, does WAL allow redo or undo of the delete?
- If crash happens after page flush but before commit, does recovery know whether the delete was committed?

---

## 7. Scan / range-read path

Although the user request names four core operations, this engine also supports scanning. It sits just above the same storage path.

```text
Scan(start_key)
        |
        v
Find first valid leaf >= start_key
        |
        +--> traverse tree to leaf with start key
        |
        v
Iterate leaf entries in order
        |
        +--> follow leaf sibling pointers as needed
        +--> merge across multiple pages if range spans more than one leaf
        |
        v
Return iterator
```

Important decisions:
- should the start key be exact or lower bound?
- does the range cross multiple leaf pages?
- should the iterator read lazily or materialize the whole range?
- what happens when a page is modified concurrently during iteration?

---

## 8. Complete layered decision checklist for one query

This is the full decision checklist a query must pass before completion.

### 8.1 Interface and intent
- Was the API called through `Get`, `Put`, `Delete`, or `Scan`?
- Was a transaction passed in?
- Does the operation require read or write semantics?

### 8.2 Validation
- Is the key well-formed?
- Is the value valid for the operation?
- Is the engine initialized and mounted?
- Is the page tree valid before the operation begins?

### 8.3 Locking
- Should this operation read shared or write exclusively?
- Is the key already locked by another transaction?
- If yes, do we wait, abort, or reject?

### 8.4 Tree navigation
- Which root/leaf path leads to the key?
- Does the key exist at all?
- Is this a leaf insertion, overwrite, or deletion?
- Does the operation require a split or merge?

### 8.5 Page handling
- Is the target page in memory?
- If not, load it from disk.
- Should the page be pinned before modification?
- Should it be marked dirty after the mutation?
- Should it be evicted later or kept for next reads?

### 8.6 Logging and durability
- Is the engine WAL-enabled?
- Is this operation transactional?
- Must a before-image and after-image be captured?
- Has WAL been flushed before the page is durable?
- Should a commit record be written?

### 8.7 Encoding and persistence
- How is the key/value encoded on the page?
- Is the slot directory valid after insert/delete?
- Is the checksum correct?
- Was the page written to the right page ID?

### 8.8 Recovery and consistency
- If the process crashes now, what is the safe state?
- Is redo needed?
- Is undo needed?
- Does the log plus page state produce a consistent recovery state?

---

## 9. Practical summary for this project

In this repository, the actual decision chain looks like this:

- Public `KVStore` contract decides the query family
- `BPlusTreeEngine` decides the tree path and page mutation logic
- `LockManager` decides if writes are safe under concurrency
- `BufferPoolManager` decides whether data is cached, evicted, or newly loaded
- `LogManager` decides what needs to be made durable and in what order
- `DiskManager` decides raw page persistence and file layout
- `RecoveryManager` decides the final consistent state after failures

So a single query is not just one function call; it is a coordinated chain of decisions across multiple subsystems.

---

## 10. One-line mental model

A Put/Get/Delete query in this engine is:

> API intent -> lock decision -> tree navigation -> page access -> mutation or read -> WAL ordering -> page serialization -> disk persistence -> recovery safety.

That is the full abstraction stack.
