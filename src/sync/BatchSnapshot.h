#pragma once
#include <functional>

#include "ShareCandidateCatalog.h"
#include "SnapshotExchange.h"

namespace yomuka::sync {
enum class BatchResult : uint8_t { Pending, Saved, Failed, Cancelled };
enum class BatchError : uint8_t { None, Source, Data, WriteFailure };
struct BatchEntry {
  size_t index;
  BatchResult result = BatchResult::Pending;
  BatchError error = BatchError::None;
};
// Each call handles one book. JSON and EPUB metadata are released before the next.
class BatchSnapshot {
  std::vector<BatchEntry> entries;
  size_t cursor = 0;

 public:
  void start(const std::vector<uint8_t>& selection);
  bool done() const { return cursor == entries.size(); }
  size_t completed() const { return cursor; }
  const std::vector<BatchEntry>& results() const { return entries; }
  void cancel();
  void step(const std::vector<ShareCandidate>& books, ReadingHistoryStore& history,
            const std::function<bool()>& cancelCheck = {});
};
}  // namespace yomuka::sync
