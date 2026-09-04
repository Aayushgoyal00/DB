#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>

#include "index/bplus_tree_engine.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"
#include "storage/log_manager.h"

namespace {

std::string Trim(const std::string& text) {
  std::size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return text.substr(begin, end - begin);
}

std::string Lowercase(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return text;
}

void PrintHelp() {
  std::cout
      << "Commands:\n"
      << "  put <key> <value>        Insert or update a key\n"
      << "  bulk_put [count]         Insert 2,000 sequential key/value pairs\n"
      << "  get <key>                Read a key\n"
      << "  delete <key>             Remove a key\n"
      << "  scan <start_key> [limit] Scan keys in sorted order\n"
      << "  pages                    Show current allocated page count\n"
      << "  help                     Show this help\n"
      << "  exit                     Quit\n";
}

}  // namespace

int main() {
  dbengine::DiskManager disk_manager("dbengine.db");
  dbengine::LogManager log_manager("dbengine.wal");
  dbengine::BufferPoolManager buffer_pool_manager(64, &disk_manager, &log_manager);
  dbengine::BPlusTreeEngine engine(&buffer_pool_manager, &log_manager, true);

  std::cout << "dbengine WAL-backed mini-CLI\n";
  std::cout << "Data file: dbengine.db\n";
  std::cout << "WAL file: dbengine.wal\n";
  std::cout << "Type 'help' for commands.\n\n";

  std::string line;
  while (true) {
    std::cout << "db> ";
    if (!std::getline(std::cin, line)) {
      std::cout << "\n";
      break;
    }
    line = Trim(line);
    if (line.empty()) {
      continue;
    }

    std::istringstream input(line);
    std::string command;
    input >> command;
    command = Lowercase(command);

    if (command == "exit" || command == "quit") {
      break;
    }

    if (command == "help") {
      PrintHelp();
      continue;
    }

    if (command == "pages") {
      std::cout << "pages=" << disk_manager.GetNumPages() << "\n";
      continue;
    }

    if (command == "bulk_put") {
      int count = 2000;
      if (input >> count && count <= 0) {
        std::cout << "count must be > 0\n";
        continue;
      }
      bool failed = false;
      for (int index = 1; index <= count; ++index) {
        const std::string key = "user:" + std::to_string(index);
        const std::string value = "value:" + std::to_string(index);
        const dbengine::Status status = engine.Put(key, value);
        if (!status.ok()) {
          std::cout << "error at " << key << ": " << status.message() << "\n";
          failed = true;
          break;
        }
      }
      if (!failed) {
        std::cout << "inserted " << count << " key/value pairs\n";
      }
      continue;
    }

    if (command == "put") {
      std::string key;
      input >> key;
      std::string value;
      std::getline(input, value);
      value = Trim(value);
      if (key.empty() || value.empty()) {
        std::cout << "usage: put <key> <value>\n";
        continue;
      }
      const dbengine::Status status = engine.Put(key, value);
      if (status.ok()) {
        std::cout << "ok\n";
      } else {
        std::cout << "error: " << status.message() << "\n";
      }
      continue;
    }

    if (command == "get") {
      std::string key;
      input >> key;
      if (key.empty()) {
        std::cout << "usage: get <key>\n";
        continue;
      }
      std::string value;
      const dbengine::Status status = engine.Get(key, &value);
      if (status.ok()) {
        std::cout << value << "\n";
      } else if (status.IsNotFound()) {
        std::cout << "(not found)\n";
      } else {
        std::cout << "error: " << status.message() << "\n";
      }
      continue;
    }

    if (command == "delete") {
      std::string key;
      input >> key;
      if (key.empty()) {
        std::cout << "usage: delete <key>\n";
        continue;
      }
      const dbengine::Status status = engine.Delete(key);
      if (status.ok()) {
        std::cout << "ok\n";
      } else if (status.IsNotFound()) {
        std::cout << "(not found)\n";
      } else {
        std::cout << "error: " << status.message() << "\n";
      }
      continue;
    }

    if (command == "scan") {
      std::string start_key;
      input >> start_key;
      if (start_key.empty()) {
        std::cout << "usage: scan <start_key> [limit]\n";
        continue;
      }
      int limit = 20;
      if (!(input >> limit)) {
        limit = 20;
      }
      if (limit <= 0) {
        std::cout << "limit must be > 0\n";
        continue;
      }
      auto iterator = engine.Scan(start_key);
      int shown = 0;
      while (iterator->Valid() && shown < limit) {
        std::cout << iterator->Key() << " = " << iterator->Value() << "\n";
        iterator->Next();
        ++shown;
      }
      if (shown == 0) {
        std::cout << "(no rows)\n";
      }
      continue;
    }

    std::cout << "unknown command: " << command << "\n";
    std::cout << "type 'help' for available commands\n";
  }

  std::cout << "bye\n";
  return 0;
}
