#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/config.h"
#include "common/status.h"
#include "storage/page.h"

namespace dbengine {

// Values are written to disk, so do not reorder them after a database file
// exists. kUninitialized is only valid for a freshly zeroed Page.
enum class PageType : uint8_t {
  kUninitialized = 0,
  kLeaf = 1,
  kInternal = 2,
  kMeta = 3,
};

struct LeafCell {
  std::string key;
  std::string value;
};

struct InternalCell {
  std::string key;
  page_id_t child_page_id;
};

// The on-page representation is explicitly little-endian:
//   leaf:     [key_len:u16][key][value_len:u16][value]
//   internal: [key_len:u16][key][child_page_id:i32]
Status EncodeLeafCell(std::string_view key, std::string_view value,
                      std::vector<char>* encoded_out);
Status DecodeLeafCell(std::span<const char> encoded, LeafCell* cell_out);
Status EncodeInternalCell(std::string_view key, page_id_t child_page_id,
                          std::vector<char>* encoded_out);
Status DecodeInternalCell(std::span<const char> encoded,
                          InternalCell* cell_out);

// A fixed-size page with variable-size cells. Slots grow forward from the
// 16-byte header and cells are allocated backward from the end of the page.
// The gap between them is contiguous free space. DeleteCell removes its slot
// but intentionally does not compact cell bytes; a future vacuum can reclaim
// that fragmentation without changing the persisted layout.
class SlottedPage {
 public:
  static constexpr uint16_t kHeaderSize = 16;
  static constexpr uint16_t kSlotSize = 4;

  explicit SlottedPage(Page* page);

  // Initializes a freshly allocated page. Calling this discards all existing
  // cells, so it is only appropriate before the page has been published.
  void Initialize(PageType page_type,
                  page_id_t right_sibling_page_id = INVALID_PAGE_ID);

  Status InsertCell(uint16_t slot_idx, std::span<const char> cell_data);
  Status DeleteCell(uint16_t slot_idx);
  std::span<const char> GetCell(uint16_t slot_idx) const;
  uint16_t NumCells() const;
  uint16_t FreeSpace() const;
  uint32_t ComputeChecksum() const;
  bool VerifyChecksum() const;

  PageType GetPageType() const;
  page_id_t RightSiblingPageId() const;

 private:
  static constexpr uint16_t kPageTypeOffset = 0;
  static constexpr uint16_t kChecksumOffset = 1;
  static constexpr uint16_t kNumCellsOffset = 5;
  static constexpr uint16_t kFreeSpaceOffsetOffset = 7;
  static constexpr uint16_t kRightSiblingOffset = 9;

  bool HasSaneLayout() const;
  uint16_t SlotOffset(uint16_t slot_idx) const;
  uint16_t SlotLength(uint16_t slot_idx) const;
  void WriteSlot(uint16_t slot_idx, uint16_t offset, uint16_t length);
  void UpdateChecksum();

  Page* page_;  // wrapped, not owned
};

}  // namespace dbengine
