#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
namespace yomuka::sync {
enum class TransferResult : uint8_t { Pending, Sent, Failed, Uncertain, Cancelled, NotSent };
// Only an acknowledged book is Sent. Cancellation never changes completed entries.
class DirectShareBatch {
  std::vector<TransferResult> entries;
  size_t cursor = 0;

 public:
  void start(size_t count) {
    entries.assign(count, TransferResult::Pending);
    cursor = 0;
  }
  bool canRetry() const {
    for (auto result : entries)
      if (result == TransferResult::Failed || result == TransferResult::Cancelled || result == TransferResult::NotSent)
        return true;
    return false;
  }
  void retry() {
    if (!canRetry()) return;
    for (auto& result : entries)
      if (result == TransferResult::Failed || result == TransferResult::Cancelled || result == TransferResult::NotSent)
        result = TransferResult::Pending;
    cursor = 0;
    while (!done() && entries[cursor] != TransferResult::Pending) ++cursor;
  }
  bool done() const { return cursor == entries.size(); }
  size_t current() const { return cursor; }
  const std::vector<TransferResult>& results() const { return entries; }
  void record(TransferResult result, bool stop = false) {
    if (done()) return;
    entries[cursor++] = result;
    while (!done() && entries[cursor] != TransferResult::Pending) ++cursor;
    if (stop)
      while (!done()) {
        if (entries[cursor] == TransferResult::Pending) entries[cursor] = TransferResult::NotSent;
        ++cursor;
      }
  }
  size_t count(TransferResult result) const {
    size_t n = 0;
    for (auto value : entries)
      if (value == result) ++n;
    return n;
  }
};
}  // namespace yomuka::sync
