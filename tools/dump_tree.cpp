#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/config.h"
#include "index/bplus_tree_engine.h"
#include "storage/disk_manager.h"
#include "storage/page.h"
#include "storage/slotted_page.h"

namespace {

using dbengine::DiskManager;
using dbengine::INVALID_PAGE_ID;
using dbengine::LeafCell;
using dbengine::Page;
using dbengine::PageType;
using dbengine::SlottedPage;
using dbengine::Status;

struct NodeRecord {
  std::string type;
  std::vector<std::string> keys;
  std::vector<dbengine::page_id_t> children;
  std::vector<std::string> values;
  std::optional<dbengine::page_id_t> next_leaf;
};

std::string JsonEscape(const std::string& value) {
  std::ostringstream out;
  for (unsigned char ch : value) {
    switch (ch) {
      case '\\':
        out << "\\\\";
        break;
      case '"':
        out << "\\\"";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (ch < 0x20) {
          out << "\\u00" << std::hex << std::uppercase << static_cast<int>(ch)
              << std::nouppercase << std::dec;
        } else {
          out << static_cast<char>(ch);
        }
        break;
    }
  }
  return out.str();
}

std::string JsonArrayOfStrings(const std::vector<std::string>& items) {
  if (items.empty()) {
    return "[]";
  }
  std::ostringstream out;
  out << "[";
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << "\"" << JsonEscape(items[i]) << "\"";
  }
  out << "]";
  return out.str();
}

std::string JsonArrayOfInts(const std::vector<dbengine::page_id_t>& items) {
  if (items.empty()) {
    return "[]";
  }
  std::ostringstream out;
  out << "[";
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << items[i];
  }
  out << "]";
  return out.str();
}

std::string JsonNullOrInt(std::optional<dbengine::page_id_t> value) {
  if (!value.has_value()) {
    return "null";
  }
  return std::to_string(*value);
}

std::string FormatPageSummary(const NodeRecord& record, dbengine::page_id_t page_id) {
  std::ostringstream out;
  out << "Page " << page_id << " [" << record.type << "]";
  if (!record.keys.empty()) {
    out << " keys: [";
    for (std::size_t i = 0; i < record.keys.size(); ++i) {
      if (i != 0) out << ", ";
      out << record.keys[i];
    }
    out << "]";
  }
  if (!record.values.empty()) {
    out << " values: [";
    for (std::size_t i = 0; i < record.values.size(); ++i) {
      if (i != 0) out << ", ";
      out << record.values[i];
    }
    out << "]";
  }
  if (!record.children.empty()) {
    out << " children: [";
    for (std::size_t i = 0; i < record.children.size(); ++i) {
      if (i != 0) out << ", ";
      out << record.children[i];
    }
    out << "]";
  }
  if (record.next_leaf.has_value()) {
    out << " next_leaf: " << *record.next_leaf;
  }
  return out.str();
}

std::string ReadMetadataRootPage(const std::filesystem::path& db_path, bool* has_tree,
                                 std::optional<dbengine::page_id_t>* root_page_out) {
  std::error_code ec;
  if (!std::filesystem::exists(db_path, ec) || !std::filesystem::is_regular_file(db_path, ec)) {
    return "DB file does not exist or is not a regular file: " + db_path.string();
  }

  DiskManager disk_manager(db_path.string());
  if (disk_manager.GetNumPages() == 0) {
    *has_tree = false;
    *root_page_out = std::nullopt;
    return "Tree is empty: no pages allocated in the database file.";
  }

  Page page;
  Status status = disk_manager.ReadPage(0, page.GetData());
  if (!status.ok()) {
    return "Failed to read metadata page 0: " + status.message();
  }

  SlottedPage metadata(&page);
  if (metadata.GetPageType() != PageType::kMeta || !metadata.VerifyChecksum()) {
    return "Metadata page 0 is invalid or corrupted.";
  }
  if (metadata.NumCells() != 1) {
    return "Metadata page 0 does not contain exactly one root record.";
  }

  const std::span<const char> record = metadata.GetCell(0);
  if (record.size() != 8 || std::memcmp(record.data(), "BPT1", 4) != 0) {
    return "Metadata record is invalid or does not match the B+Tree magic value.";
  }

  const uint32_t root_value =
      static_cast<uint32_t>(static_cast<uint8_t>(record.data()[4])) |
      (static_cast<uint32_t>(static_cast<uint8_t>(record.data()[5])) << 8) |
      (static_cast<uint32_t>(static_cast<uint8_t>(record.data()[6])) << 16) |
      (static_cast<uint32_t>(static_cast<uint8_t>(record.data()[7])) << 24);
  const dbengine::page_id_t root_page = static_cast<dbengine::page_id_t>(root_value);
  if (root_page == INVALID_PAGE_ID) {
    *has_tree = false;
    *root_page_out = std::nullopt;
    return "Tree is empty: root page ID is not set in metadata.";
  }

  *has_tree = true;
  *root_page_out = root_page;
  return "";
}

void WalkPage(DiskManager& disk_manager,
              dbengine::page_id_t page_id,
              std::unordered_map<dbengine::page_id_t, NodeRecord>* nodes,
              std::set<dbengine::page_id_t>* visited,
              std::vector<std::string>* errors) {
  if (page_id == INVALID_PAGE_ID) {
    return;
  }
  if (!visited->insert(page_id).second) {
    return;
  }

  Page page;
  Status status = disk_manager.ReadPage(page_id, page.GetData());
  if (!status.ok()) {
    errors->push_back("Error reading page " + std::to_string(page_id) + ": " + status.message());
    return;
  }

  SlottedPage slotted(&page);
  if (!slotted.VerifyChecksum()) {
    errors->push_back("Checksum failed for page " + std::to_string(page_id) + ".");
    return;
  }

  const PageType page_type = slotted.GetPageType();
  if (page_type == PageType::kLeaf) {
    NodeRecord record;
    record.type = "leaf";
    for (uint16_t i = 0; i < slotted.NumCells(); ++i) {
      dbengine::LeafCell cell;
      Status decode_status = dbengine::DecodeLeafCell(slotted.GetCell(i), &cell);
      if (!decode_status.ok()) {
        errors->push_back("Failed to decode leaf cell " + std::to_string(i) + " on page " +
                          std::to_string(page_id) + ": " + decode_status.message());
        return;
      }
      record.keys.push_back(cell.key);
      record.values.push_back(cell.value);
    }
    if (slotted.RightSiblingPageId() != INVALID_PAGE_ID) {
      record.next_leaf = slotted.RightSiblingPageId();
    }
    (*nodes)[page_id] = std::move(record);
    return;
  }

  if (page_type == PageType::kInternal) {
    NodeRecord record;
    record.type = "internal";
    const auto leftmost_child = slotted.RightSiblingPageId();
    if (leftmost_child == INVALID_PAGE_ID) {
      errors->push_back("Internal page " + std::to_string(page_id) + " has no leftmost child.");
      (*nodes)[page_id] = record;
      return;
    }
    record.children.push_back(leftmost_child);

    for (uint16_t i = 0; i < slotted.NumCells(); ++i) {
      dbengine::InternalCell cell;
      Status decode_status = dbengine::DecodeInternalCell(slotted.GetCell(i), &cell);
      if (!decode_status.ok()) {
        errors->push_back("Failed to decode internal cell " + std::to_string(i) + " on page " +
                          std::to_string(page_id) + ": " + decode_status.message());
        return;
      }
      record.keys.push_back(cell.key);
      record.children.push_back(cell.child_page_id);
    }
    (*nodes)[page_id] = std::move(record);
    for (dbengine::page_id_t child : (*nodes)[page_id].children) {
      WalkPage(disk_manager, child, nodes, visited, errors);
    }
    return;
  }

  if (page_type == PageType::kMeta) {
    errors->push_back("Page " + std::to_string(page_id) + " is metadata, not a tree node.");
    return;
  }

  errors->push_back("Unexpected page type on page " + std::to_string(page_id) + ": " +
                    std::to_string(static_cast<int>(page_type)) + ".");
}

void EmitJson(const std::optional<dbengine::page_id_t>& root_page,
              const std::unordered_map<dbengine::page_id_t, NodeRecord>& nodes) {
  std::cout << "{\n";
  if (root_page.has_value()) {
    std::cout << "  \"root_page\": " << *root_page << ",\n";
  } else {
    std::cout << "  \"root_page\": null,\n";
  }
  std::cout << "  \"nodes\": {\n";

  bool first = true;
  for (const auto& [page_id, record] : nodes) {
    if (!first) {
      std::cout << ",\n";
    }
    first = false;

    std::cout << "    \"" << page_id << "\": {\n";
    std::cout << "      \"type\": \"" << record.type << "\",";
    std::cout << "\n      \"keys\": " << JsonArrayOfStrings(record.keys) << ",\n";
    if (record.type == "internal") {
      std::cout << "      \"children\": " << JsonArrayOfInts(record.children) << ",\n";
    } else {
      std::cout << "      \"values\": " << JsonArrayOfStrings(record.values) << ",\n";
      std::cout << "      \"next_leaf\": " << JsonNullOrInt(record.next_leaf) << "\n";
    }
    std::cout << "    }";
  }

  std::cout << "\n  }\n";
  std::cout << "}\n";
}

void EmitPrettyTree(const std::optional<dbengine::page_id_t>& root_page,
                   const std::unordered_map<dbengine::page_id_t, NodeRecord>& nodes) {
  if (!root_page.has_value()) {
    std::cout << "Tree is empty: no root page is set in metadata.\n";
    return;
  }

  const auto root_it = nodes.find(*root_page);
  if (root_it == nodes.end()) {
    std::cout << "Root page " << *root_page << " was not readable or did not decode into a node.\n";
    return;
  }

  std::set<dbengine::page_id_t> visited;
  std::function<void(dbengine::page_id_t, const std::string&)> walk =
      [&](dbengine::page_id_t page_id, const std::string& indent) {
        auto it = nodes.find(page_id);
        if (it == nodes.end()) {
          std::cout << indent << "Page " << page_id << " [unreadable]\n";
          return;
        }
        const NodeRecord& record = it->second;
        std::cout << indent << FormatPageSummary(record, page_id) << "\n";
        if (record.type == "internal") {
          for (std::size_t i = 0; i < record.children.size(); ++i) {
            const dbengine::page_id_t child = record.children[i];
            if (visited.insert(child).second) {
              std::cout << indent << "  ├─ child " << child << "\n";
              walk(child, indent + "  │   ");
            }
          }
        }
      };

  std::cout << "Root (page " << *root_page << ")\n";
  walk(*root_page, "");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    std::cerr << "Usage: dump_tree <path_to_db_file> [--pretty]\n";
    return 1;
  }

  const std::string db_path = argv[1];
  bool pretty = false;
  if (argc == 3) {
    const std::string mode = argv[2];
    if (mode == "--pretty") {
      pretty = true;
    } else {
      std::cerr << "Unknown option: " << mode << "\nUsage: dump_tree <path_to_db_file> [--pretty]\n";
      return 2;
    }
  }

  std::optional<dbengine::page_id_t> root_page;
  bool has_tree = false;
  std::string metadata_error = ReadMetadataRootPage(db_path, &has_tree, &root_page);
  if (!metadata_error.empty()) {
    std::cerr << metadata_error << "\n";
    if (pretty) {
      std::cout << "Tree is empty: no root page is set in metadata.\n";
    } else {
      std::cout << "{\n  \"root_page\": null,\n  \"nodes\": {}\n}\n";
    }
    return 0;
  }

  if (!has_tree || !root_page.has_value()) {
    std::cerr << "Tree is empty: no root page is set in metadata.\n";
    if (pretty) {
      std::cout << "Tree is empty: no root page is set in metadata.\n";
    } else {
      std::cout << "{\n  \"root_page\": null,\n  \"nodes\": {}\n}\n";
    }
    return 0;
  }

  DiskManager disk_manager(db_path);
  std::unordered_map<dbengine::page_id_t, NodeRecord> nodes;
  std::set<dbengine::page_id_t> visited;
  std::vector<std::string> errors;
  WalkPage(disk_manager, *root_page, &nodes, &visited, &errors);

  for (const std::string& error : errors) {
    std::cerr << error << "\n";
  }

  if (pretty) {
    EmitPrettyTree(root_page, nodes);
  } else {
    EmitJson(root_page, nodes);
  }

  return 0;
}
