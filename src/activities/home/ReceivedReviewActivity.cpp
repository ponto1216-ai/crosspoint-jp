#include "ReceivedReviewActivity.h"

#include <HalPowerManager.h>
#include <I18n.h>

#include <cstdio>

#include "BatchImportActivity.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"
using namespace yomuka::sync;
namespace {
constexpr unsigned long ProgressRenderIntervalMs = 3000;
}
const char* ReceivedReviewActivity::status(const ReceivedBook& book) const {
  if (book.status == ReceivedStatus::Matched) return tr(STR_RECEIVED_MATCH);
  if (book.status == ReceivedStatus::Missing) return tr(STR_RECEIVED_MISSING);
  if (book.error == ReceivedError::Source) return tr(STR_SHARE_SOURCE_ERROR);
  if (book.error == ReceivedError::Range) return tr(STR_RECEIVED_RANGE);
  if (book.error == ReceivedError::Memory) return tr(STR_DIRECT_MEMORY_ERROR);
  if (book.error == ReceivedError::Search) return tr(STR_RECEIVED_INCOMPLETE);
  if (book.error == ReceivedError::File) return tr(STR_RECEIVED_FILE_ERROR);
  if (book.error == ReceivedError::Snapshot) return tr(STR_RECEIVED_INVALID);
  return tr(STR_RECEIVED_CHECKING);
}
std::vector<std::string> ReceivedReviewActivity::details(const ReceivedBook& book) const {
  std::vector<std::string> lines{book.title, status(book)};
  char id[17];
  snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(book.id));
  if (book.id) lines.push_back(std::string("BookId: ") + id);
  if (!book.path.empty()) lines.push_back(book.path);
  if (book.units & 1)
    lines.push_back(std::string(tr(STR_SHARE_POSITION)) + ": " + std::to_string(book.spineIndex + 1) + " / " +
                    std::to_string(book.chapterPage + 1));
  if (book.units & 2) lines.push_back(std::string(tr(STR_SHARE_BOOKMARKS)) + ": " + std::to_string(book.bookmarks));
  if (book.units & 4) lines.push_back(tr(STR_SHARE_SETTINGS));
  if (book.units & 8) lines.push_back(tr(STR_SHARE_HISTORY));
  lines.push_back(tr(STR_DIRECT_NO_IMPORT));
  lines.push_back(book.status == ReceivedStatus::Matched ? tr(STR_DIRECT_IMPORT_LATER)
                                                         : tr(STR_RECEIVED_CANNOT_IMPORT));
  return lines;
}
void ReceivedReviewActivity::loop() {
  const bool skipBack = ignoreBack, skipConfirm = ignoreConfirm;
  if (!mappedInput.isPressed(MappedInputManager::Button::Back)) ignoreBack = false;
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) ignoreConfirm = false;
  if (!skipBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!detail) {
      finish();
      return;
    }
    RenderLock lock;
    detail = false;
    requestUpdate();
    return;
  }
  if (!scanner.done()) {
    RenderLock lock;
    HalPowerManager::Lock power;
    scanner.step([this, skipBack] {
      mappedInput.update();
      return !skipBack && mappedInput.isPressed(MappedInputManager::Button::Back);
    });
    if (scanner.done() || millis() - lastUpdate >= ProgressRenderIntervalMs) {
      lastUpdate = millis();
      requestUpdate();
    }
    return;
  }
  if (scanner.books().empty()) return;
  RenderLock lock;
  const bool canImport = scanner.count(ReceivedStatus::Matched) > 0;
  const auto count = detail ? details(scanner.books()[cursor]).size() : scanner.books().size() + (canImport ? 1 : 0);
  navigator.onNextRelease([this, count] {
    auto& index = detail ? detailCursor : cursor;
    index = ButtonNavigator::nextIndex(index, count);
    requestUpdate();
  });
  navigator.onPreviousRelease([this, count] {
    auto& index = detail ? detailCursor : cursor;
    index = ButtonNavigator::previousIndex(index, count);
    requestUpdate();
  });
  if (!skipConfirm && !detail && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (cursor == scanner.books().size()) {
      startActivityForResult(std::make_unique<BatchImportActivity>(renderer, mappedInput, scanner.books()),
                             [](const ActivityResult&) {});
      ignoreConfirm = true;
      ignoreBack = true;
      return;
    }
    detail = true;
    detailCursor = 0;
    requestUpdate();
  }
}
void ReceivedReviewActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto layout = UiLayout::from(renderer);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool inverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int headerY = layout.content.y + metrics.topPadding + (inverted ? metrics.buttonHintsHeight : 0);
  const int top = headerY + metrics.headerHeight + metrics.verticalSpacing;
  const int line = renderer.getLineHeight(UI_10_FONT_ID) + 8;
  const int offset = layout.content.x + layout.content.width / 2 - renderer.getScreenWidth() / 2;
  auto text = [&](int y, const std::string& value) {
    auto t = renderer.truncatedText(UI_10_FONT_ID, value.c_str(), layout.content.width - 20);
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, y, t.c_str(), true, offset);
  };
  GUI.drawHeader(renderer, Rect{layout.content.x, headerY, layout.content.width, metrics.headerHeight},
                 tr(STR_RECEIVED_TITLE));
  const int bottom = layout.content.y + layout.content.height - metrics.buttonHintsHeight - line;
  if (!scanner.done()) {
    text(top, tr(STR_RECEIVED_CHECKING));
    text(top + line, std::to_string(scanner.checkedFiles));
    text(top + line * 3, tr(STR_SHARE_CANCEL_HINT));
  } else if (scanner.books().empty()) {
    text(top, tr(STR_RECEIVED_EMPTY));
    if (scanner.warnings) text(top + line, tr(STR_RECEIVED_INCOMPLETE));
  } else if (detail) {
    const auto lines = details(scanner.books()[cursor]);
    GUI.drawList(renderer, Rect{layout.content.x, top, layout.content.width, bottom - top - line}, lines.size(),
                 detailCursor, [&](int i) { return lines[i]; });
  } else {
    text(top, std::string(tr(STR_RECEIVED_MATCH)) + ": " + std::to_string(scanner.count(ReceivedStatus::Matched)) +
                  " / " + tr(STR_RECEIVED_MISSING) + ": " + std::to_string(scanner.count(ReceivedStatus::Missing)) +
                  " / " + tr(STR_RECEIVED_ERROR) + ": " + std::to_string(scanner.count(ReceivedStatus::Error)));
    GUI.drawList(
        renderer, Rect{layout.content.x, top + line, layout.content.width, bottom - top - line * 2},
        scanner.books().size() + (scanner.count(ReceivedStatus::Matched) ? 1 : 0), cursor,
        [this](int i) {
          return i < static_cast<int>(scanner.books().size()) ? scanner.books()[i].title
                                                              : std::string(tr(STR_IMPORT_SELECT));
        },
        [this](int i) {
          return i < static_cast<int>(scanner.books().size()) ? std::string("　") + status(scanner.books()[i])
                                                              : std::string();
        });
    text(bottom, scanner.warnings ? tr(STR_RECEIVED_INCOMPLETE) : tr(STR_DIRECT_NO_IMPORT));
  }
  auto labels =
      mappedInput.mapLabels(tr(STR_BACK),
                            scanner.done() && !scanner.books().empty() && !detail
                                ? (cursor == scanner.books().size() ? tr(STR_CONFIRM) : tr(STR_RECEIVED_DETAIL))
                                : "",
                            scanner.done() ? tr(STR_DIR_UP) : "", scanner.done() ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
