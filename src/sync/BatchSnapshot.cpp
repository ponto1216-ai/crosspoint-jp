#include "BatchSnapshot.h"

#include "SnapshotBookProbe.h"

namespace yomuka::sync {
void BatchSnapshot::start(const std::vector<uint8_t>& selection) {
  entries.clear();
  cursor = 0;
  for (size_t i = 0; i < selection.size(); ++i)
    if (selection[i]) entries.push_back({i});
}
void BatchSnapshot::cancel() {
  while (!done()) entries[cursor++].result = BatchResult::Cancelled;
}
void BatchSnapshot::step(const std::vector<ShareCandidate>& books, ReadingHistoryStore& history,
                         const std::function<bool()>& cancelCheck) {
  if (done()) return;
  auto& entry = entries[cursor++];
  bool cancelled = false;
  const auto stop = [&] {
    if (!cancelled && cancelCheck) cancelled = cancelCheck();
    if (cancelled) {
      entry.result = BatchResult::Cancelled;
      entry.error = BatchError::None;
      cancel();
    }
    return cancelled;
  };
  if (stop()) return;
  entry.result = BatchResult::Failed;
  if (entry.index >= books.size()) {
    entry.error = BatchError::Source;
    return;
  }
  const auto& candidate = books[entry.index];
  ExchangeBook book{candidate.bookId, 0, candidate.path, candidate.title, ""};
  if (!candidate.resolved() || !probeSnapshotBook(book.path, book.id, book.spines, stop)) {
    if (stop()) return;
    entry.error = BatchError::Source;
    return;
  }
  JsonDocument snapshot;
  if (stop()) return;
  if (!candidate.units || candidate.errors ||
      exportSnapshot(book, candidate.units, history, snapshot) != ExchangeError::None) {
    entry.error = BatchError::Data;
    return;
  }
  if (stop()) return;
  // Use the existing bounded validator and verified single-file replacement.
  // No write touches reader progress, bookmarks, overrides or history imports.
  if (stageSnapshotForBook(book, snapshot) != ExchangeError::None) {
    entry.error = BatchError::WriteFailure;
    return;
  }
  entry.result = BatchResult::Saved;
  // Publication finishes atomically; cancel only the remaining books afterward.
  if (cancelCheck && cancelCheck()) cancel();
}
}  // namespace yomuka::sync
