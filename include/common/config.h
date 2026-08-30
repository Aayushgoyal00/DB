#pragma once

#include <cstdint>

namespace dbengine {

// Fixed page size for the whole engine. 4KB matches common OS page/sector
// sizes, which keeps a single page's I/O aligned to one disk read/write.
// This will matter later for buffer pool + WAL work (Phase 3) — changing it
// after data exists on disk is a format-breaking change, so pin it now.
constexpr size_t PAGE_SIZE = 4096;

// Page ids are just offsets into the db file (page_id * PAGE_SIZE = byte
// offset). int32_t caps us at ~2^31 pages ≈ 8TB at 4KB pages, plenty for a
// learning project.
using page_id_t = int32_t;
constexpr page_id_t INVALID_PAGE_ID = -1;

// Buffer pool frame id — index into the in-memory frame array. Introduced
// here so later phases don't need to touch this header again.
using frame_id_t = int32_t;

// Log sequence number — monotonically increasing position in the WAL.
// u64 because a busy engine writes millions of records per second and we
// never want this to wrap in any realistic deployment.
using lsn_t = uint64_t;
constexpr lsn_t INVALID_LSN = 0;

// Transaction id. u32 is plenty for a single-node engine.
using txn_id_t = uint32_t;
constexpr txn_id_t INVALID_TXN_ID = 0;

} // namespace dbengine
