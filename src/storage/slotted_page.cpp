#include "storage/slotted_page.h"

#include <cstring>
#include <limits>

namespace dbengine {
namespace {

uint16_t ReadU16(const char* data) {
  return static_cast<uint16_t>(static_cast<uint8_t>(data[0])) |
         (static_cast<uint16_t>(static_cast<uint8_t>(data[1])) << 8);
}

uint32_t ReadU32(const char* data) {
  return static_cast<uint32_t>(static_cast<uint8_t>(data[0])) |
         (static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(data[3])) << 24);
}

void WriteU16(char* data, uint16_t value) {
  data[0] = static_cast<char>(value & 0xffU);
  data[1] = static_cast<char>((value >> 8) & 0xffU);
}

void WriteU32(char* data, uint32_t value) {
  data[0] = static_cast<char>(value & 0xffU);
  data[1] = static_cast<char>((value >> 8) & 0xffU);
  data[2] = static_cast<char>((value >> 16) & 0xffU);
  data[3] = static_cast<char>((value >> 24) & 0xffU);
}

uint32_t Crc32Byte(uint32_t crc, uint8_t byte) {
  crc ^= byte;
  for (int bit = 0; bit < 8; ++bit) {
    crc = (crc & 1U) != 0 ? (crc >> 1) ^ 0xedb88320U : crc >> 1;
  }
  return crc;
}

Status CheckCellSize(std::size_t key_size, std::size_t value_size,
                     std::size_t fixed_size) {
  if (key_size > std::numeric_limits<uint16_t>::max() ||
      value_size > std::numeric_limits<uint16_t>::max() ||
      fixed_size + key_size + value_size > std::numeric_limits<uint16_t>::max()) {
    return Status::InvalidArgument("cell is too large for the page format");
  }
  return Status::OK();
}

}  // namespace

Status EncodeLeafCell(std::string_view key, std::string_view value,
                      std::vector<char>* encoded_out) {
  if (encoded_out == nullptr) {
    return Status::InvalidArgument("encoded_out must not be null");
  }
  Status size_status = CheckCellSize(key.size(), value.size(), 4);
  if (!size_status.ok()) {
    return size_status;
  }
  encoded_out->resize(4 + key.size() + value.size());
  char* data = encoded_out->data();
  WriteU16(data, static_cast<uint16_t>(key.size()));
  std::memcpy(data + 2, key.data(), key.size());
  WriteU16(data + 2 + key.size(), static_cast<uint16_t>(value.size()));
  std::memcpy(data + 4 + key.size(), value.data(), value.size());
  return Status::OK();
}

Status DecodeLeafCell(std::span<const char> encoded, LeafCell* cell_out) {
  if (cell_out == nullptr) {
    return Status::InvalidArgument("cell_out must not be null");
  }
  if (encoded.size() < 4) {
    return Status::Corruption("leaf cell is smaller than its length fields");
  }
  const uint16_t key_length = ReadU16(encoded.data());
  if (encoded.size() < 4U + key_length) {
    return Status::Corruption("leaf key length exceeds cell size");
  }
  const uint16_t value_length = ReadU16(encoded.data() + 2 + key_length);
  if (encoded.size() != 4U + key_length + value_length) {
    return Status::Corruption("leaf value length does not match cell size");
  }
  cell_out->key.assign(encoded.data() + 2, key_length);
  cell_out->value.assign(encoded.data() + 4 + key_length, value_length);
  return Status::OK();
}

Status EncodeInternalCell(std::string_view key, page_id_t child_page_id,
                          std::vector<char>* encoded_out) {
  if (encoded_out == nullptr) {
    return Status::InvalidArgument("encoded_out must not be null");
  }
  Status size_status = CheckCellSize(key.size(), 0, 6);
  if (!size_status.ok()) {
    return size_status;
  }
  encoded_out->resize(6 + key.size());
  char* data = encoded_out->data();
  WriteU16(data, static_cast<uint16_t>(key.size()));
  std::memcpy(data + 2, key.data(), key.size());
  WriteU32(data + 2 + key.size(), static_cast<uint32_t>(child_page_id));
  return Status::OK();
}

Status DecodeInternalCell(std::span<const char> encoded,
                          InternalCell* cell_out) {
  if (cell_out == nullptr) {
    return Status::InvalidArgument("cell_out must not be null");
  }
  if (encoded.size() < 6) {
    return Status::Corruption("internal cell is smaller than its fields");
  }
  const uint16_t key_length = ReadU16(encoded.data());
  if (encoded.size() != 6U + key_length) {
    return Status::Corruption("internal key length does not match cell size");
  }
  cell_out->key.assign(encoded.data() + 2, key_length);
  cell_out->child_page_id =
      static_cast<page_id_t>(ReadU32(encoded.data() + 2 + key_length));
  return Status::OK();
}

SlottedPage::SlottedPage(Page* page) : page_(page) {}

void SlottedPage::Initialize(PageType page_type, page_id_t right_sibling_page_id) {
  page_->ResetMemory();
  char* data = page_->GetData();
  data[kPageTypeOffset] = static_cast<char>(page_type);
  WriteU16(data + kNumCellsOffset, 0);
  WriteU16(data + kFreeSpaceOffsetOffset, PAGE_SIZE);
  WriteU32(data + kRightSiblingOffset,
           static_cast<uint32_t>(right_sibling_page_id));
  UpdateChecksum();
}

Status SlottedPage::InsertCell(uint16_t slot_idx, std::span<const char> cell_data) {
  if (!HasSaneLayout()) {
    return Status::Corruption("InsertCell: invalid page header or slots");
  }
  const uint16_t num_cells = NumCells();
  if (slot_idx > num_cells) {
    return Status::InvalidArgument("InsertCell: slot index out of range");
  }
  if (cell_data.empty()) {
    return Status::InvalidArgument("InsertCell: empty cells are not supported");
  }
  if (cell_data.size() > std::numeric_limits<uint16_t>::max() ||
      cell_data.size() + kSlotSize > FreeSpace()) {
    return Status::InvalidArgument("InsertCell: page full");
  }

  char* data = page_->GetData();
  const uint16_t cell_offset =
      static_cast<uint16_t>(ReadU16(data + kFreeSpaceOffsetOffset) - cell_data.size());
  std::memcpy(data + cell_offset, cell_data.data(), cell_data.size());

  const std::size_t slot_bytes_to_move =
      static_cast<std::size_t>(num_cells - slot_idx) * kSlotSize;
  char* insertion_point = data + kHeaderSize + slot_idx * kSlotSize;
  std::memmove(insertion_point + kSlotSize, insertion_point, slot_bytes_to_move);
  WriteSlot(slot_idx, cell_offset, static_cast<uint16_t>(cell_data.size()));
  WriteU16(data + kNumCellsOffset, static_cast<uint16_t>(num_cells + 1));
  WriteU16(data + kFreeSpaceOffsetOffset, cell_offset);
  UpdateChecksum();
  return Status::OK();
}

Status SlottedPage::DeleteCell(uint16_t slot_idx) {
  if (!HasSaneLayout()) {
    return Status::Corruption("DeleteCell: invalid page header or slots");
  }
  const uint16_t num_cells = NumCells();
  if (slot_idx >= num_cells) {
    return Status::InvalidArgument("DeleteCell: slot index out of range");
  }

  char* data = page_->GetData();
  char* deletion_point = data + kHeaderSize + slot_idx * kSlotSize;
  const std::size_t slot_bytes_to_move =
      static_cast<std::size_t>(num_cells - slot_idx - 1) * kSlotSize;
  std::memmove(deletion_point, deletion_point + kSlotSize, slot_bytes_to_move);
  std::memset(data + kHeaderSize + (num_cells - 1) * kSlotSize, 0, kSlotSize);
  WriteU16(data + kNumCellsOffset, static_cast<uint16_t>(num_cells - 1));
  if (num_cells == 1) {
    // Once every cell is gone, all of the formerly fragmented cell space is
    // again contiguous and can be returned immediately.
    WriteU16(data + kFreeSpaceOffsetOffset, PAGE_SIZE);
  }
  UpdateChecksum();
  return Status::OK();
}

std::span<const char> SlottedPage::GetCell(uint16_t slot_idx) const {
  if (!HasSaneLayout() || slot_idx >= NumCells()) {
    return {};
  }
  const uint16_t offset = SlotOffset(slot_idx);
  const uint16_t length = SlotLength(slot_idx);
  return {page_->GetData() + offset, length};
}

uint16_t SlottedPage::NumCells() const {
  return ReadU16(page_->GetData() + kNumCellsOffset);
}

uint16_t SlottedPage::FreeSpace() const {
  if (!HasSaneLayout()) {
    return 0;
  }
  const uint16_t slot_end = static_cast<uint16_t>(kHeaderSize + NumCells() * kSlotSize);
  return static_cast<uint16_t>(ReadU16(page_->GetData() + kFreeSpaceOffsetOffset) - slot_end);
}

uint32_t SlottedPage::ComputeChecksum() const {
  // CRC-32/ISO-HDLC across every byte except the checksum field itself. This
  // includes page_type, unlike a checksum over only the body, so corruption
  // of any serialized byte is detectable.
  const char* data = page_->GetData();
  uint32_t crc = 0xffffffffU;
  for (uint16_t i = 0; i < PAGE_SIZE; ++i) {
    if (i < kChecksumOffset || i >= kChecksumOffset + sizeof(uint32_t)) {
      crc = Crc32Byte(crc, static_cast<uint8_t>(data[i]));
    }
  }
  return ~crc;
}

bool SlottedPage::VerifyChecksum() const {
  return ReadU32(page_->GetData() + kChecksumOffset) == ComputeChecksum();
}

PageType SlottedPage::GetPageType() const {
  return static_cast<PageType>(static_cast<uint8_t>(page_->GetData()[kPageTypeOffset]));
}

page_id_t SlottedPage::RightSiblingPageId() const {
  return static_cast<page_id_t>(ReadU32(page_->GetData() + kRightSiblingOffset));
}

bool SlottedPage::HasSaneLayout() const {
  if (page_ == nullptr) {
    return false;
  }
  const uint16_t num_cells = NumCells();
  const uint16_t free_space_offset =
      ReadU16(page_->GetData() + kFreeSpaceOffsetOffset);
  const std::size_t slot_end = kHeaderSize + static_cast<std::size_t>(num_cells) * kSlotSize;
  if (slot_end > PAGE_SIZE || free_space_offset < slot_end ||
      free_space_offset > PAGE_SIZE) {
    return false;
  }
  for (uint16_t i = 0; i < num_cells; ++i) {
    const uint16_t offset = SlotOffset(i);
    const uint16_t length = SlotLength(i);
    if (length == 0 || offset < free_space_offset ||
        static_cast<std::size_t>(offset) + length > PAGE_SIZE) {
      return false;
    }
  }
  return true;
}

uint16_t SlottedPage::SlotOffset(uint16_t slot_idx) const {
  return ReadU16(page_->GetData() + kHeaderSize + slot_idx * kSlotSize);
}

uint16_t SlottedPage::SlotLength(uint16_t slot_idx) const {
  return ReadU16(page_->GetData() + kHeaderSize + slot_idx * kSlotSize + sizeof(uint16_t));
}

void SlottedPage::WriteSlot(uint16_t slot_idx, uint16_t offset, uint16_t length) {
  char* slot = page_->GetData() + kHeaderSize + slot_idx * kSlotSize;
  WriteU16(slot, offset);
  WriteU16(slot + sizeof(uint16_t), length);
}

void SlottedPage::UpdateChecksum() {
  WriteU32(page_->GetData() + kChecksumOffset, ComputeChecksum());
}

}  // namespace dbengine
