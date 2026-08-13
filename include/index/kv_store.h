#pragma once

#include <memory>
#include <string>

#include "common/status.h"

namespace dbengine {

// Forward-declared here, defined properly once Phase 2/5 need real
// iteration. Kept minimal on purpose — an Iterator is just "has a current
// key/value and can advance," nothing engine-specific belongs here.
class Iterator {
 public:
  virtual ~Iterator() = default;
  virtual bool Valid() const = 0;
  virtual void Next() = 0;
  virtual const std::string& Key() const = 0;
  virtual const std::string& Value() const = 0;
};

// The one interface every storage engine implements: B+Tree (Phase 2),
// its copy-on-write variant (Phase 4), and the LSM engine (Phase 5) are all
// interchangeable behind this. Nothing above this layer (a CLI, a future
// MCP server wrapper, benchmark harness) should ever depend on which engine
// is plugged in — see ARCHITECTURE.md section 3.
class KVStore {
 public:
  virtual ~KVStore() = default;

  virtual Status Get(const std::string& key, std::string* value_out) = 0;
  virtual Status Put(const std::string& key, const std::string& value) = 0;
  virtual Status Delete(const std::string& key) = 0;

  // Returns an iterator positioned at the first key >= start_key.
  // An empty start_key means "from the beginning."
  virtual std::unique_ptr<Iterator> Scan(const std::string& start_key) = 0;
};

} // namespace dbengine
