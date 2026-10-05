#include <algorithm>
#include <iostream>

#include "sync/BatchImport.h"
using namespace yomuka::sync;
#define CHECK(x)                                   \
  do {                                             \
    if (!(x)) {                                    \
      std::cerr << __LINE__ << ": " << #x << "\n"; \
      return 1;                                    \
    }                                              \
  } while (0)
int main() {
  std::vector<ReceivedBook> books(4);
  for (size_t i = 0; i < books.size(); ++i) {
    books[i].id = i + 1;
    books[i].status = ReceivedStatus::Matched;
  }
  books[3].status = ReceivedStatus::Missing;
  BatchImport batch;
  batch.start(books, {1, 1, 1, 1});
  CHECK(batch.results().size() == 3);
  std::vector<unsigned> calls(4, 0);
  bool failing = true;
  auto apply = [&](const ReceivedBook& b, const std::function<bool()>&) {
    ++calls[b.id - 1];
    return b.id == 2 && failing ? ImportOutcome{ImportResult::Failed, ImportError::Font}
                                : ImportOutcome{ImportResult::Imported, ImportError::None, true};
  };
  while (!batch.done()) batch.step(books, apply, [] { return false; });
  CHECK(batch.results()[0].outcome.committed && batch.results()[1].outcome.result == ImportResult::Failed &&
        batch.results()[2].outcome.committed);
  CHECK(batch.canRetry());
  failing = false;
  batch.retry();
  while (!batch.done()) batch.step(books, apply, [] { return false; });
  CHECK(calls[0] == 1 && calls[1] == 2 && calls[2] == 1 && calls[3] == 0 && !batch.canRetry());
  batch.start(books, {1, 1, 1});
  batch.step(books, apply, [] { return false; });
  batch.cancel();
  CHECK(batch.results()[0].outcome.committed && batch.results()[1].outcome.result == ImportResult::Cancelled);
  batch.retry();
  while (!batch.done()) batch.step(books, apply, [] { return false; });
  CHECK(batch.results()[0].outcome.committed);
  batch.start(books, {1, 1, 1});
  bool stop = false;
  batch.step(
      books,
      [&](const ReceivedBook&, const std::function<bool()>&) {
        stop = true;
        return ImportOutcome{ImportResult::Imported, ImportError::None, true};
      },
      [&] { return stop; });
  CHECK(batch.done() && batch.results()[0].outcome.committed &&
        batch.results()[1].outcome.result == ImportResult::Cancelled);
  batch.start(books, {1, 1, 1});
  batch.step(
      books,
      [](const ReceivedBook&, const std::function<bool()>&) {
        return ImportOutcome{ImportResult::RestartRequired, ImportError::Reload, true};
      },
      [] { return false; });
  CHECK(batch.done() && batch.blocked() && !batch.canRetry() && batch.results()[0].outcome.committed);
  batch.retry();
  CHECK(batch.done());
  batch.start(books, {0, 0, 0, 1});
  CHECK(batch.done());
  batch.start(books, {1});
  batch.step(books, apply, [] { return true; });
  CHECK(batch.done() && batch.results()[0].outcome.result == ImportResult::Cancelled);
  std::cout << "PASS: per-book results, partial success, retry only failed/cancelled, committed cancellation, restart "
               "blocking, missing excluded\n";
}
