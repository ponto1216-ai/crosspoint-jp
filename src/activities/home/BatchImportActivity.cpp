#include "BatchImportActivity.h"

#include <HalPowerManager.h>
#include <I18n.h>

#include <algorithm>

#include "components/UITheme.h"
#include "components/ActionListDivider.h"
#include "components/UiLayout.h"
#include "fontIds.h"
#include "sync/ReceivedSnapshotImport.h"
#include "sync/ReviewNavigation.h"
#include "sync/SnapshotComparison.h"
using namespace yomuka::sync;
BatchImportActivity::BatchImportActivity(GfxRenderer& r, MappedInputManager& i, const std::vector<ReceivedBook>& source)
    : Activity("BatchImport", r, i), books(source) {
  for (size_t index = 0; index < books.size(); ++index)
    if (books[index].status == ReceivedStatus::Matched) rows.push_back(index);
  selection.resize(books.size(), 0);
}
int BatchImportActivity::reviewRowsPerPage() const {
  const auto layout = UiLayout::from(renderer);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int line = renderer.getLineHeight(UI_10_FONT_ID) + 8;
  const bool inverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int top = layout.content.y + metrics.topPadding + (inverted ? metrics.buttonHintsHeight : 0) +
                  metrics.headerHeight + metrics.verticalSpacing;
  const int bottom = layout.content.y + layout.content.height - metrics.buttonHintsHeight - line;
  return std::max(1, (bottom - top) / line);
}
void BatchImportActivity::prepareReview() {
  HalPowerManager::Lock power;
  reviewLines.clear();
  const int width = UiLayout::from(renderer).content.width - 20;
  for (const auto& original : compareReceivedBook(books[reviewBooks[reviewIndex]])) {
    std::string line;
    for (size_t at = 0; at < original.size();) {
      size_t end = at + 1;
      while (end < original.size() && (static_cast<unsigned char>(original[end]) & 0xc0) == 0x80) ++end;
      const auto glyph = original.substr(at, end - at);
      if (!line.empty() && renderer.getTextWidth(UI_10_FONT_ID, (line + glyph).c_str()) > width) {
        reviewLines.push_back(line);
        line.clear();
      }
      line += glyph;
      at = end;
    }
    reviewLines.push_back(line);
  }
  cursor = 0;
}
void BatchImportActivity::beginReview(bool retry) {
  retrying = retry;
  reviewBooks.clear();
  reviewIndex = 0;
  if (retry) {
    for (const auto& entry : batch.results())
      if (entry.outcome.result == ImportResult::Failed || entry.outcome.result == ImportResult::Cancelled)
        reviewBooks.push_back(entry.index);
  } else {
    for (const auto index : rows)
      if (selection[index]) reviewBooks.push_back(index);
  }
  if (reviewBooks.empty()) return;
  screen = Screen::Review;
  prepareReview();
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
    if (screen == Screen::Confirm || screen == Screen::Review) {
      RenderLock lock;
      screen = retrying ? Screen::Results : Screen::Select;
      cursor = 0;
      requestUpdate();
    } else
      finish();
    return;
  }
  RenderLock lock;
  const size_t count = screen == Screen::Review    ? finalReviewPage(reviewLines.size(), reviewRowsPerPage()) + 1
                       : screen == Screen::Select  ? rows.size() + 1
                       : screen == Screen::Results ? batch.results().size() + (batch.canRetry() ? 1 : 0)
                                                   : 1;
  navigator.onNextRelease([this, count] {
    cursor = screen == Screen::Review ? std::min(cursor + 1, count - 1) : ButtonNavigator::nextIndex(cursor, count);
    requestUpdate();
  });
  navigator.onPreviousRelease([this, count] {
    cursor = screen == Screen::Review ? (cursor ? cursor - 1 : 0) : ButtonNavigator::previousIndex(cursor, count);
    requestUpdate();
  });
  if (skipConfirm || !mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return;
  if (screen == Screen::Select) {
    if (cursor < rows.size())
      selection[rows[cursor]] ^= 1;
    else if (std::count(selection.begin(), selection.end(), 1)) {
      beginReview(false);
    }
  } else if (screen == Screen::Review) {
    if (cursor == static_cast<size_t>(finalReviewPage(reviewLines.size(), reviewRowsPerPage()))) {
      if (reviewIndex + 1 < reviewBooks.size()) {
        ++reviewIndex;
        prepareReview();
      } else {
        screen = Screen::Confirm;
        cursor = 0;
      }
    }
  } else if (screen == Screen::Confirm) {
    if (retrying)
      batch.retry();
    else
      batch.start(books, selection);
    screen = Screen::Running;
    cursor = 0;
  } else if (screen == Screen::Results && cursor == batch.results().size() && batch.canRetry()) {
    beginReview(true);
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
    const Rect listRect{layout.content.x, top, layout.content.width, bottom - top - line};
    GUI.drawList(
        renderer, listRect, rows.size() + 1, cursor,
        [this](int i) {
          return i < static_cast<int>(rows.size())
                     ? std::string(selection[rows[i]] ? "[選択] " : "[  ] ") + books[rows[i]].title
                     : std::string(tr(STR_IMPORT_REVIEW));
        },
        [this](int i) {
          return i < static_cast<int>(rows.size()) ? std::string("　") + tr(STR_RECEIVED_MATCH) : std::string();
        });
    drawActionListDivider(renderer, listRect, metrics.listWithSubtitleRowHeight, rows.size(), cursor);
    text(bottom, std::to_string(std::count(selection.begin(), selection.end(), 1)) + " " + tr(STR_SHARE_SELECTED));
  } else if (screen == Screen::Review) {
    const int perPage = reviewRowsPerPage();
    const int finalPage = finalReviewPage(reviewLines.size(), perPage);
    if (cursor < static_cast<size_t>(finalPage)) {
      const size_t first = cursor * perPage, end = std::min(reviewLines.size(), first + perPage);
      for (size_t i = first; i < end; ++i) text(top + (i - first) * line, reviewLines[i]);
    } else {
      text(top, books[reviewBooks[reviewIndex]].title);
      text(top + line * 2, tr(STR_IMPORT_COMPARE_END));
      text(top + line * 3, tr(STR_IMPORT_COMPARE_NEXT));
    }
    text(bottom, std::to_string(reviewIndex + 1) + " / " + std::to_string(reviewBooks.size()) + "冊・" +
                     std::to_string(cursor + 1) + " / " + std::to_string(finalPage + 1) + "頁");
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
    const Rect listRect{layout.content.x, top + line, layout.content.width, bottom - top - line * 2};
    GUI.drawList(
        renderer, listRect,
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
    if (batch.canRetry())
      drawActionListDivider(renderer, listRect, metrics.listWithSubtitleRowHeight, batch.results().size(), cursor);
    if (batch.blocked()) text(bottom, tr(STR_IMPORT_RESTART));
  }
  auto labels = mappedInput.mapLabels(
      batch.blocked() ? "" : tr(STR_BACK),
      batch.blocked() || screen == Screen::Running ? ""
      : screen == Screen::Review
          ? (cursor == static_cast<size_t>(finalReviewPage(reviewLines.size(), reviewRowsPerPage()))
                 ? tr(STR_IMPORT_COMPARE_NEXT_SHORT)
                 : "")
      : screen == Screen::Confirm                                    ? tr(STR_SHARE_WRITE_SHORT)
      : screen == Screen::Select && cursor < rows.size()             ? tr(STR_SELECT)
      : screen == Screen::Results && cursor < batch.results().size() ? ""
                                                                     : tr(STR_CONFIRM),
      screen == Screen::Running || batch.blocked() ? "" : tr(STR_DIR_UP),
      screen == Screen::Running || batch.blocked() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
