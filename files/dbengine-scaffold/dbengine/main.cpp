#include <iostream>

#include "index/bplus_tree_engine.h"
#include "storage/disk_manager.h"

int main() {
  dbengine::DiskManager disk_manager("dbengine.db");
  dbengine::BPlusTreeEngine engine(&disk_manager);

  std::cout << "dbengine Phase 0 skeleton is wired up. "
            << "Pages allocated so far: " << disk_manager.GetNumPages()
            << "\n";
  return 0;
}
