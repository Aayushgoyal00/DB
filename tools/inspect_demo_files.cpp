#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

static std::string ReadStringFromWire(const std::string& data, std::size_t* pos) {
  if (*pos + 4 > data.size()) {
    return "";
  }
  const uint32_t len = static_cast<uint32_t>(
      (static_cast<unsigned char>(data[*pos]) |
       (static_cast<unsigned char>(data[*pos + 1]) << 8) |
       (static_cast<unsigned char>(data[*pos + 2]) << 16) |
       (static_cast<unsigned char>(data[*pos + 3]) << 24)));
  *pos += 4;
  if (*pos + len > data.size()) {
    return "";
  }
  const std::string value = data.substr(*pos, len);
  *pos += len;
  return value;
}

static std::string TypeName(uint8_t type) {
  switch (type) {
    case 0: return "BEGIN";
    case 1: return "COMMIT";
    case 2: return "ABORT";
    case 3: return "INSERT";
    case 4: return "UPDATE";
    case 5: return "DELETE";
    case 6: return "CHECKPOINT";
    default: return "UNKNOWN";
  }
}

struct DecodedLogFrame {
  uint32_t body_len = 0;
  uint32_t crc = 0;
  uint8_t type = 0;
  uint32_t txn_id = 0;
  int32_t page_id = 0;
  std::string key;
  std::string before_image;
  std::string after_image;
};

static std::vector<DecodedLogFrame> ReadFrames(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::cout << "Could not open log file: " << path << "\n";
    return {};
  }

  std::vector<DecodedLogFrame> frames;
  while (in) {
    uint32_t body_len = 0;
    in.read(reinterpret_cast<char*>(&body_len), sizeof(body_len));
    if (in.gcount() == 0) {
      break;
    }
    if (in.gcount() != sizeof(body_len)) {
      std::cout << "Incomplete body length in WAL.\n";
      break;
    }

    std::string body(body_len, '\0');
    in.read(body.data(), static_cast<std::streamsize>(body_len));
    if (in.gcount() != static_cast<std::streamsize>(body_len)) {
      std::cout << "Incomplete body in WAL.\n";
      break;
    }

    uint32_t crc = 0;
    in.read(reinterpret_cast<char*>(&crc), sizeof(crc));
    if (in.gcount() != sizeof(crc)) {
      std::cout << "Incomplete CRC trailer in WAL.\n";
      break;
    }

    std::size_t pos = 0;
    if (body.empty()) {
      frames.push_back({body_len, crc, 0, 0, 0, "", "", ""});
      continue;
    }

    const uint8_t type = static_cast<uint8_t>(body[pos++]);
    uint32_t txn = 0;
    int32_t page = 0;
    if (pos + 4 <= body.size()) {
      txn = static_cast<uint32_t>(
          (static_cast<unsigned char>(body[pos]) |
           (static_cast<unsigned char>(body[pos + 1]) << 8) |
           (static_cast<unsigned char>(body[pos + 2]) << 16) |
           (static_cast<unsigned char>(body[pos + 3]) << 24)));
      pos += 4;
    }
    if (pos + 4 <= body.size()) {
      page = static_cast<int32_t>(
          (static_cast<unsigned char>(body[pos]) |
           (static_cast<unsigned char>(body[pos + 1]) << 8) |
           (static_cast<unsigned char>(body[pos + 2]) << 16) |
           (static_cast<unsigned char>(body[pos + 3]) << 24)));
      pos += 4;
    }

    const std::string key = ReadStringFromWire(body, &pos);
    const std::string before = ReadStringFromWire(body, &pos);
    const std::string after = ReadStringFromWire(body, &pos);

    frames.push_back({body_len, crc, type, txn, page, key, before, after});
  }

  return frames;
}

static void PrintDbSummary(const std::string& db_path) {
  std::ifstream in(db_path, std::ios::binary | std::ios::ate);
  if (!in) {
    std::cout << "Database file not found: " << db_path << "\n";
    return;
  }

  const std::streamoff size = in.tellg();
  std::cout << "=== Database file summary ===\n";
  std::cout << "Path: " << db_path << "\n";
  std::cout << "Size: " << size << " bytes\n";
  std::cout << "Note: this is a binary B+ tree file, not plain text.\n";
  std::cout << "Use the project dump_tree tool to inspect its structure.\n";

  in.seekg(0, std::ios::beg);
  std::vector<unsigned char> head(64, 0);
  in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));

  std::cout << "First 64 bytes (hex):\n";
  for (std::size_t i = 0; i < head.size(); ++i) {
    std::cout << std::hex << std::setw(2) << std::setfill('0')
              << static_cast<int>(head[i]) << ' ';
    if ((i + 1) % 16 == 0) {
      std::cout << std::dec << "\n";
    }
  }
  std::cout << "\n";
}

static void PrintLogSummary(const std::string& log_path) {
  const auto frames = ReadFrames(log_path);

  std::cout << "=== WAL log summary ===\n";
  std::cout << "Path: " << log_path << "\n";
  std::cout << "Frames found: " << frames.size() << "\n\n";

  for (std::size_t i = 0; i < frames.size(); ++i) {
    const auto& f = frames[i];
    std::cout << "Frame " << i << "\n";
    std::cout << "  type       = " << TypeName(f.type) << "\n";
    std::cout << "  txn_id     = " << f.txn_id << "\n";
    std::cout << "  page_id    = " << f.page_id << "\n";
    std::cout << "  key        = " << (f.key.empty() ? "(empty)" : f.key) << "\n";
    std::cout << "  before     = " << (f.before_image.empty() ? "(empty)" : f.before_image) << "\n";
    std::cout << "  after      = " << (f.after_image.empty() ? "(empty)" : f.after_image) << "\n";
    std::cout << "  body_len   = " << f.body_len << "\n";
    std::cout << "  crc        = " << f.crc << "\n\n";
  }

  if (frames.empty()) {
    std::cout << "No WAL frames were found. Run the benchmark first to generate them.\n";
  }
}

int main(int argc, char** argv) {
  std::string db_path = "demo_db.db";
  std::string log_path = "demo_db.log";

  if (argc > 1) {
    db_path = argv[1];
  }
  if (argc > 2) {
    log_path = argv[2];
  }

  std::cout << "Inspecting generated database files...\n\n";
  PrintDbSummary(db_path);
  std::cout << "\n";
  PrintLogSummary(log_path);
  return 0;
}
