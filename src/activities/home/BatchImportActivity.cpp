#include "BatchImportActivity.h"

#include <I18n.h>

#include <algorithm>

#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"
#include "sync/ReceivedSnapshotImport.h"
using namespace yomuka::sync;
BatchImportActivity::BatchImportActivity(GfxRenderer& r, MappedInputManager& i, const std::vector<ReceivedBook>& source)
    : Activity("BatchImport", r, i), books(source) {
  for (size_t index = 0; index < books.size(); ++index)
    if (books[index].status == ReceivedStatus::Matched) rows.push_back(index);
  selection.resize(books.size(), 0);
}
const char* BatchImportActivity::resultText(const ImportOutcome& outcome) const {
  if (outcome.result == ImportResult::Imported) return tr(STR_IMPORT_DONE);
  if (outcome.result == ImportResult::Cancelled) return tr(STR_SHARE_CANCELLED);
  if (outcome.result == ImportResult::RestartRequired)
    return outcome.committed ? tr(STR_IMPORT_APPLIED_RESTART) : tr(STR_IMPORT_RESTART);
  if (outcome.result == ImportResult::Pending) return tr(STR_IMPORT_WAITING);
  if (outcome.error == ImportError::Font) return tr(STR_IMPORT_FONT_ERROR);
  if (outcome.error == ImportError::Source) return tr(STR_SHARE_SOURCE_ERROR);
  if (outcome.error == ImportError::File) return tr(STR_RECEIVED_FILE_ERROR);
  if (outcome.error == ImportError::Layout) return tr(STR_IMPORT_LAYOUT_ERROR);
  return tr(STR_IMPORT_STORAGE_ERROR);
}
void BatchImportActivity::loop() {
  const bool skipBack = ignoreBack, skipConfirm = ignoreConfirm;
  if (!mappedInput.isPressed(MappedInputManager::Button::Back)) ignoreBack = false;
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) ignoreConfirm = false;
  if (batch.blocked()) return;
  if (screen == Screen::Running) {
    if (!skipBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      RenderLock lock;
      batch.cancel();
      screen = Screen::Results;
      cursor = 0;
      requestUpdate();
      return;
    }
    // Publish progress before taking the render/power locks for a book.
    requestUpdateAndWait();
    batch.step(
        books,
        [this](const ReceivedBook& book, const std::function<bool()>& stop) {
          return importReceivedSnapshot(book, renderer, stop);
        },
        [this, skipBack] {
          mappedInput.update();
          return !skipBack && mappedInput.isPressed(MappedInputManager::Button::Back);
        });
    RenderLock lock;
    if (batch.done()) {
      screen = Screen::Results;
      cursor = 0;
    }
    // Consume a Back held during preparation/commit, not the result screen.
    if (mappedInput.isPressed(MappedInputManager::Button::Back)) ignoreBack = true;
    requestUpdate();
    return;
  }
  if (!skipBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (screen == Screen::Confirm) {
      RenderLock lock;
      screen = retrying ? Screen::Results : Screen::Select;
      cursor = 0;
      requestUpdate();
    } else
      finish();
    return;
  }
  RenderLock lock;
  const size_t count = screen == Screen::Select    ? rows.size() + 1
                       : screen == Screen::Results ? batch.results().size() + (batch.canRetry() ? 1 : 0)
                                                   : 1;
  navigator.onNextRelease([this, count] {
    cursor = ButtonNavigator::nextIndex(cursor, count);
    requestUpdate();
  });
  navigator.onPreviousRelease([this, count] {
    cursor = ButtonNavigator::previousIndex(cursor, count);
    requestUpdate();
  });
  if (skipConfirm || !mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return;
  if (screen == Screen::Select) {
    if (cursor < rows.size())
      selection[rows[cursor]] ^= 1;
    else if (std::count(selection.begin(), selection.end(), 1)) {
      screen = Screen::Confirm;
      retrying = false;
      cursor = 0;
    }
  } else if (screen == Screen::Confirm) {
    if (retrying)
      batch.retry();
    else
      batch.start(books, selection);
    screen = Screen::Running;
    cursor = 0;
  } else if (screen == Screen::Results && cursor == batch.results().size() && batch.canRetry()) {
    retrying = true;
    screen = Screen::Confirm;
    cursor = 0;
  }
  requestUpdate();
}
void BatchImportActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto layout = UiLayout::from(renderer);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool inverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int headerY = layout.content.y + metrics.topPadding + (inverted ? metrics.buttonHintsHeight : 0);
  const int top = headerY + metrics.headerHeight + metrics.verticalSpacing;
  const int line = renderer.getLineHeight(UI_10_FONT_ID) + 8;
  const int bottom = layout.content.y + layout.content.height - metrics.buttonHintsHeight - line;
  const int offset = layout.content.x + layout.content.width / 2 - renderer.getScreenWidth() / 2;
  auto text = [&](int y, const std::string& value) {
    auto clipped = renderer.truncatedText(UI_10_FONT_ID, value.c_str(), layout.content.width - 20);
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, y, clipped.c_str(), true, offset);
  };
  GUI.drawHeader(renderer, Rect{layout.content.x, headerY, layout.content.width, metrics.headerHeight},
                 tr(STR_IMPORT_TITLE));
  if (screen == Screen::Select) {
    GUI.drawList(
        renderer, Rect{layout.content.x, top, layout.content.width, bottom - top - line}, rows.size() + 1, cursor,
        [this](int i) {
          return i < static_cast<int>(rows.size())
                     ? std::string(selection[rows[i]] ? "[選択] " : "[  ] ") + books[rows[i]].title
                     : std::string(tr(STR_IMPORT_REVIEW));
        },
        [this](int i) {
          return i < static_cast<int>(rows.size()) ? std::string("　") + tr(STR_RECEIVED_MATCH) : std::string();
        });
    text(bottom, std::to_string(std::count(selection.begin(), selection.end(), 1)) + " " + tr(STR_SHARE_SELECTED));
  } else if (screen == Screen::Confirm) {
    text(top, retrying ? tr(STR_IMPORT_RETRY_CONFIRM) : tr(STR_IMPORT_CONFIRM));
    text(top + line * 2, tr(STR_IMPORT_REPLACE));
    text(top + line * 3, tr(STR_IMPORT_EMPTY_REPLACE));
    text(top + line * 4, tr(STR_IMPORT_ABSENT_UNCHANGED));
    text(top + line * 5, tr(STR_IMPORT_APPROX));
  } else if (screen == Screen::Running) {
    text(top, tr(STR_IMPORT_RUNNING));
    text(top + line * 2, std::to_string(batch.completed()) + " / " + std::to_string(batch.results().size()));
    if (batch.completed() < batch.results().size())
      text(top + line * 3, books[batch.results()[batch.completed()].index].title);
    text(top + line * 5, tr(STR_SHARE_CANCEL_HINT));
  } else {
    size_t success = 0, failed = 0, cancelled = 0;
    for (const auto& entry : batch.results()) {
      success += entry.outcome.committed;
      failed += !entry.outcome.committed &&
                (entry.outcome.result == ImportResult::Failed || entry.outcome.result == ImportResult::RestartRequired);
      cancelled += entry.outcome.result == ImportResult::Cancelled;
    }
    text(top, std::string(tr(STR_SHARE_SUCCESS)) + ": " + std::to_string(success) + " / " + tr(STR_SHARE_FAILED) +
                  ": " + std::to_string(failed) + " / " + tr(STR_SHARE_CANCELLED) + ": " + std::to_string(cancelled));
    GUI.drawList(
        renderer, Rect{layout.content.x, top + line, layout.content.width, bottom - top - line * 2},
        batch.results().size() + (batch.canRetry() ? 1 : 0), cursor,
        [this](int i) {
          return i < static_cast<int>(batch.results().size()) ? books[batch.results()[i].index].title
                                                              : std::string(tr(STR_IMPORT_RETRY));
        },
        [this](int i) {
          return i < static_cast<int>(batch.results().size())
                     ? std::string("　") + resultText(batch.results()[i].outcome)
                     : std::string();
        });
    if (batch.blocked()) text(bottom, tr(STR_IMPORT_RESTART));
  }
  auto labels = mappedInput.mapLabels(batch.blocked() ? "" : tr(STR_BACK),
                                      batch.blocked() || screen == Screen::Running       ? ""
                                      : screen == Screen::Confirm                        ? tr(STR_SHARE_WRITE_SHORT)
                                      : screen == Screen::Select && cursor < rows.size() ? tr(STR_SELECT)
                                      : screen == Screen::Results && cursor < batch.results().size() ? ""
                                                                                                     : tr(STR_CONFIRM),
                                      screen == Screen::Running || batch.blocked() ? "" : tr(STR_DIR_UP),
                                      screen == Screen::Running || batch.blocked() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
