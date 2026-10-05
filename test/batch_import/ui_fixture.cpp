#include <algorithm>
#include <iostream>

#include "sync/BatchImport.h"
#include "sync/ReviewNavigation.h"
using namespace yomuka::sync;
#define CHECK(x)                                   \
  do {                                             \
    if (!(x)) {                                    \
      std::cerr << __LINE__ << ": " << #x << "\n"; \
      return 1;                                    \
    }                                              \
  } while (0)
struct MappedInputManager {
  enum class Button { Back, Confirm, Left, Right, None };
  Button held = Button::None, released = Button::None;
  bool isPressed(Button b) { return held == b; }
  bool wasReleased(Button b) { return released == b; }
  void update() {}
};
using B = MappedInputManager::Button;
struct RenderLock {
  ~RenderLock() {}
};
struct ButtonNavigator {
  MappedInputManager& i;
  template <class F>
  void onNextRelease(F f) {
    if (i.wasReleased(B::Right)) f();
  }
  template <class F>
  void onPreviousRelease(F f) {
    if (i.wasReleased(B::Left)) f();
  }
  static size_t nextIndex(size_t i, size_t n) { return (i + 1) % n; }
  static size_t previousIndex(size_t i, size_t n) { return (i + n - 1) % n; }
};
int calls[3] = {};
bool failSecond = true;
ImportOutcome importReceivedSnapshot(const ReceivedBook& b, int&, const std::function<bool()>& stop) {
  if (stop()) return {ImportResult::Cancelled};
  ++calls[b.id - 1];
  if (b.id == 2 && failSecond) return {ImportResult::Failed, ImportError::Font};
  return {ImportResult::Imported, ImportError::None, true};
}
struct BatchImportActivity {
  enum class Screen { Select, Review, Confirm, Running, Results };
  Screen screen = Screen::Select;
  std::vector<ReceivedBook> books;
  std::vector<size_t> rows{0, 1, 2};
  std::vector<uint8_t> selection;
  std::vector<size_t> reviewBooks;
  std::vector<std::string> reviewLines;
  size_t reviewIndex = 0;
  int reviewRowsPerPage() const { return 3; }
  void prepareReview() {
    reviewLines = {"title", "position", "dates", "warning"};
    cursor = 0;
  }
  void beginReview(bool retry) {
    retrying = retry;
    reviewBooks.clear();
    reviewIndex = 0;
    if (retry) {
      for (const auto& entry : batch.results())
        if (entry.outcome.result == ImportResult::Failed || entry.outcome.result == ImportResult::Cancelled)
          reviewBooks.push_back(entry.index);
    } else
      for (auto index : rows)
        if (selection[index]) reviewBooks.push_back(index);
    screen = Screen::Review;
    prepareReview();
  }
  void finishReview() {
    while (screen == Screen::Review) {
      cursor = finalReviewPage(reviewLines.size(), reviewRowsPerPage());
      loop();
    }
  }

  BatchImport batch;
  ButtonNavigator navigator;
  MappedInputManager& mappedInput;
  int renderer = 0;
  size_t cursor = 0;
  bool ignoreConfirm = true, ignoreBack = true, retrying = false, finished = false;
  explicit BatchImportActivity(MappedInputManager& i) : navigator{i}, mappedInput(i) {
    books.resize(3);
    selection.resize(3);
    for (size_t x = 0; x < 3; ++x) {
      books[x].id = x + 1;
      books[x].status = ReceivedStatus::Matched;
    }
  }
  void finish() { finished = true; }
  void requestUpdate() {}
  void requestUpdateAndWait() {}
  void loop();
};
// ACTUAL_LOOP
int main() {
  MappedInputManager i;
  BatchImportActivity a(i);
  a.loop();
  i.released = B::Confirm;
  a.loop();
  CHECK(a.selection[0] && calls[0] == 0);
  i.released = B::Right;
  a.loop();
  i.released = B::Confirm;
  a.loop();
  CHECK(a.selection[1]);
  a.cursor = 3;
  i.released = B::Confirm;
  a.loop();
  CHECK(a.screen == BatchImportActivity::Screen::Review && calls[0] == 0);
  i.released = B::Left;
  a.loop();
  CHECK(a.cursor == 0);
  i.released = B::Confirm;
  a.loop();
  CHECK(a.screen == BatchImportActivity::Screen::Review && calls[0] == 0);
  i.released = B::Back;
  a.loop();
  CHECK(a.screen == BatchImportActivity::Screen::Select);
  a.cursor = 3;
  i.released = B::Confirm;
  a.loop();
  a.finishReview();
  a.loop();
  CHECK(a.screen == BatchImportActivity::Screen::Running);
  i.released = B::None;
  a.loop();
  a.loop();
  CHECK(a.screen == BatchImportActivity::Screen::Results && calls[0] == 1 && calls[1] == 1);
  a.cursor = a.batch.results().size();
  i.released = B::Confirm;
  a.loop();
  CHECK(a.screen == BatchImportActivity::Screen::Review && a.reviewBooks.size() == 1 && a.reviewBooks[0] == 1);
  a.finishReview();
  failSecond = false;
  a.loop();
  i.released = B::None;
  while (a.screen == BatchImportActivity::Screen::Running) a.loop();
  CHECK(calls[0] == 1 && calls[1] == 2);
  MappedInputManager j;
  BatchImportActivity cancelled(j);
  cancelled.loop();
  cancelled.selection = {1, 1, 1};
  cancelled.cursor = 3;
  j.released = B::Confirm;
  cancelled.loop();
  cancelled.finishReview();
  cancelled.loop();
  j.released = B::None;
  j.held = B::Back;
  cancelled.loop();
  CHECK(cancelled.screen == BatchImportActivity::Screen::Results && !cancelled.finished);
  j.held = B::None;
  j.released = B::Back;
  cancelled.loop();
  CHECK(!cancelled.finished);
  j.released = B::None;
  cancelled.loop();
  j.released = B::Back;
  cancelled.loop();
  CHECK(cancelled.finished);
  std::cout << "PASS: selection and explicit confirmation, confirm Back unchanged, results, failed-only retry, "
               "cancellation release consumed\n";
}
