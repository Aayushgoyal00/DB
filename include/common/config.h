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

} // namespace dbengine
