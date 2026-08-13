#pragma once

#include <string>
#include <utility>

namespace dbengine {

// A LevelDB/RocksDB-style Status object. Every public API in this engine
// returns one of these instead of throwing, so callers are forced to check
// results at every layer (disk I/O, page access, tree traversal) rather
// than relying on stack unwinding to surface disk errors. This matters more
// here than in typical app code because a swallowed I/O error can silently
// corrupt on-disk state.
class Status {
 public:
  enum class Code {
    kOk,
    kNotFound,
    kIOError,
    kCorruption,
    kInvalidArgument,
    kNotImplemented,
  };

  Status() : code_(Code::kOk) {}
  Status(Code code, std::string message)
      : code_(code), message_(std::move(message)) {}

  static Status OK() { return Status(); }
  static Status NotFound(std::string msg) {
    return Status(Code::kNotFound, std::move(msg));
  }
  static Status IOError(std::string msg) {
    return Status(Code::kIOError, std::move(msg));
  }
  static Status Corruption(std::string msg) {
    return Status(Code::kCorruption, std::move(msg));
  }
  static Status InvalidArgument(std::string msg) {
    return Status(Code::kInvalidArgument, std::move(msg));
  }
  static Status NotImplemented(std::string msg) {
    return Status(Code::kNotImplemented, std::move(msg));
  }

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsIOError() const { return code_ == Code::kIOError; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }

  Code code() const { return code_; }
  const std::string& message() const { return message_; }

 private:
  Code code_;
  std::string message_;
};

} // namespace dbengine
