#include "BatchImport.h"
namespace yomuka::sync {
void BatchImport::start(const std::vector<ReceivedBook>& books, const std::vector<uint8_t>& selection) {
  entries.clear();
  cursor = 0;
  halted = false;
  for (size_t i = 0; i < books.size() && i < selection.size(); ++i)
    if (selection[i] && books[i].status == ReceivedStatus::Matched) entries.push_back({i});
}
bool BatchImport::canRetry() const {
  if (halted) return false;
  for (const auto& entry : entries)
    if (entry.outcome.result == ImportResult::Failed || entry.outcome.result == ImportResult::Cancelled) return true;
  return false;
}
void BatchImport::retry() {
  if (!canRetry()) return;
  for (auto& entry : entries)
    if (entry.outcome.result == ImportResult::Failed || entry.outcome.result == ImportResult::Cancelled)
      entry.outcome = {ImportResult::Pending};
  cursor = 0;
  while (!done() && entries[cursor].outcome.result != ImportResult::Pending) ++cursor;
}
void BatchImport::cancel() {
  while (!done()) {
    auto& entry = entries[cursor++];
    if (entry.outcome.result == ImportResult::Pending) entry.outcome = {ImportResult::Cancelled};
  }
}
void BatchImport::step(const std::vector<ReceivedBook>& books, const Apply& apply, const std::function<bool()>& stop) {
  while (!done() && entries[cursor].outcome.result != ImportResult::Pending) ++cursor;
  if (done()) return;
  if (stop && stop()) {
    cancel();
    return;
  }
  auto& entry = entries[cursor++];
  if (entry.index >= books.size() || books[entry.index].status != ReceivedStatus::Matched)
    entry.outcome = {ImportResult::Failed, ImportError::Source};
  else
    entry.outcome = apply(books[entry.index], stop);
  if (entry.outcome.result == ImportResult::RestartRequired) {
    halted = true;
    cancel();
  } else if (entry.outcome.result == ImportResult::Cancelled || (stop && stop()))
    cancel();
}
}  // namespace yomuka::sync
