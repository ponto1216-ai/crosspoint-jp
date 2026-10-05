#pragma once
#include <functional>

#include "ReceivedBookScanner.h"
namespace yomuka::sync {
enum class ImportResult : uint8_t { Pending, Imported, Failed, Cancelled, RestartRequired };
enum class ImportError : uint8_t { None, Source, File, Font, Layout, StorageIo, Recovery, Reload };
struct ImportOutcome {
  ImportResult result = ImportResult::Failed;
  ImportError error = ImportError::None;
  bool committed = false;
};
struct ImportEntry {
  size_t index;
  ImportOutcome outcome{ImportResult::Pending};
};
class BatchImport {
  std::vector<ImportEntry> entries;
  size_t cursor = 0;
  bool halted = false;

 public:
  using Apply = std::function<ImportOutcome(const ReceivedBook&, const std::function<bool()>&)>;
  void start(const std::vector<ReceivedBook>& books, const std::vector<uint8_t>& selection);
  void retry();
  void cancel();
  void step(const std::vector<ReceivedBook>& books, const Apply& apply, const std::function<bool()>& stop);
  bool done() const { return cursor >= entries.size(); }
  bool blocked() const { return halted; }
  bool canRetry() const;
  size_t completed() const { return cursor; }
  const std::vector<ImportEntry>& results() const { return entries; }
};
}  // namespace yomuka::sync
