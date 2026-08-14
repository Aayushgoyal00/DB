#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "common/config.h"
#include "storage/slotted_page.h"

using dbengine::DecodeInternalCell;
using dbengine::DecodeLeafCell;
using dbengine::EncodeInternalCell;
using dbengine::EncodeLeafCell;
using dbengine::InternalCell;
using dbengine::LeafCell;
using dbengine::PAGE_SIZE;
using dbengine::Page;
using dbengine::PageType;
using dbengine::SlottedPage;
using dbengine::Status;

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& what) {
  if (!condition) {
    std::cerr << "FAIL: " << what << "\n";
    ++g_failures;
  } else {
    std::cout << "PASS: " << what << "\n";
  }
}

bool EqualBytes(std::span<const char> left, std::span<const char> right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin());
}

void TestCellCodecs() {
  const std::string binary_key{"a\0b", 3};
  const std::string binary_value{"value\0with\0nul", 14};
  std::vector<char> leaf_bytes;
  LeafCell leaf;
  Check(EncodeLeafCell(binary_key, binary_value, &leaf_bytes).ok(),
        "encode a binary leaf cell");
  Check(DecodeLeafCell(leaf_bytes, &leaf).ok() && leaf.key == binary_key &&
            leaf.value == binary_value,
        "leaf cell encode/decode round-trips exactly");

  std::vector<char> internal_bytes;
  InternalCell internal;
  Check(EncodeInternalCell(binary_key, -42, &internal_bytes).ok(),
        "encode an internal cell");
  Check(DecodeInternalCell(internal_bytes, &internal).ok() &&
            internal.key == binary_key && internal.child_page_id == -42,
        "internal cell encode/decode round-trips exactly");

  leaf_bytes.pop_back();
  Check(DecodeLeafCell(leaf_bytes, &leaf).IsCorruption(),
        "leaf decoder rejects a truncated cell");
  internal_bytes.push_back('x');
  Check(DecodeInternalCell(internal_bytes, &internal).IsCorruption(),
        "internal decoder rejects trailing bytes");
}

void TestSlottedPageRoundTrip() {
  Page page;
  SlottedPage slotted(&page);
  slotted.Initialize(PageType::kLeaf, 17);
  Check(slotted.GetPageType() == PageType::kLeaf && slotted.RightSiblingPageId() == 17,
        "page header persists type and right sibling");

  std::vector<std::vector<char>> cells{
      {'a'}, {'b', 'b', 'b'}, {'c', '\0', 'c', 'c'}, std::vector<char>(401, 'd')};
  bool inserted = true;
  for (uint16_t i = 0; i < cells.size(); ++i) {
    inserted = slotted.InsertCell(i, cells[i]).ok() && inserted;
  }
  Check(inserted, "insert varying-size cells");
  Check(slotted.NumCells() == cells.size(), "slot count equals inserted cells");

  bool round_trip = true;
  for (uint16_t i = 0; i < cells.size(); ++i) {
    round_trip = EqualBytes(slotted.GetCell(i), cells[i]) && round_trip;
  }
  Check(round_trip, "each slotted cell is byte-identical after insertion");

  const std::vector<char> inserted_in_middle{'x', 'y'};
  Check(slotted.InsertCell(2, inserted_in_middle).ok(), "insert at a middle slot");
  Check(EqualBytes(slotted.GetCell(2), inserted_in_middle) &&
            EqualBytes(slotted.GetCell(3), cells[2]),
        "middle insertion shifts slots without changing cell bytes");

  Check(slotted.DeleteCell(2).ok() && slotted.NumCells() == cells.size() &&
            EqualBytes(slotted.GetCell(2), cells[2]),
        "deletion removes only the requested slot");
  Check(slotted.VerifyChecksum(), "valid slotted page verifies its checksum");
}

void TestCapacityAndChecksum() {
  Page page;
  SlottedPage slotted(&page);
  slotted.Initialize(PageType::kInternal);
  const std::vector<char> cell(97, 'z');
  uint16_t inserted = 0;
  while (slotted.InsertCell(inserted, cell).ok()) {
    ++inserted;
  }
  Check(inserted > 0 && slotted.FreeSpace() < cell.size() + SlottedPage::kSlotSize,
        "fill a page until no cell and slot fit");
  const uint16_t count_before_rejection = slotted.NumCells();
  Check(!slotted.InsertCell(inserted, cell).ok() &&
            slotted.NumCells() == count_before_rejection && slotted.VerifyChecksum(),
        "a full page rejects insertion without corrupting data");

  Page checksum_source;
  SlottedPage source(&checksum_source);
  source.Initialize(PageType::kLeaf);
  const std::vector<char> checksum_cell{'c', 'e', 'l', 'l'};
  Check(source.InsertCell(0, checksum_cell).ok(), "create page for checksum test");
  bool detects_all_single_byte_changes = true;
  for (std::size_t i = 0; i < PAGE_SIZE; ++i) {
    Page corrupted = checksum_source;
    corrupted.GetData()[i] ^= static_cast<char>(0x5a);
    SlottedPage corrupt_page(&corrupted);
    detects_all_single_byte_changes = !corrupt_page.VerifyChecksum() &&
                                      detects_all_single_byte_changes;
  }
  Check(detects_all_single_byte_changes,
        "checksum detects every single-byte corruption in the serialized page");
}

}  // namespace

int main() {
  TestCellCodecs();
  TestSlottedPageRoundTrip();
  TestCapacityAndChecksum();

  if (g_failures == 0) {
    std::cout << "\nAll checks passed.\n";
    return 0;
  }
  std::cout << "\n" << g_failures << " check(s) failed.\n";
  return 1;
}
