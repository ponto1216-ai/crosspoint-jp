#include "ShareCandidatesActivity.h"

#include <HalPowerManager.h>
#include <I18n.h>

#include <algorithm>

#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"

using namespace yomuka::sync;
namespace {
constexpr uint8_t Units[] = {SavedPosition, SavedBookmarks, SavedSettings, SavedHistory};
constexpr StrId UnitLabels[] = {StrId::STR_SHARE_POSITION, StrId::STR_SHARE_BOOKMARKS, StrId::STR_SHARE_SETTINGS,
                                StrId::STR_SHARE_HISTORY};
}  // namespace
void ShareCandidatesActivity::onEnter() {
  Activity::onEnter();
  startedAt = millis();
  requestUpdate();
}
void ShareCandidatesActivity::loop() {
  if (consumeBackRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Back)) consumeBackRelease = false;
    return;
  }
  // The transfer chooser opens this activity on press; consume that key's release.
  const bool ignoreConfirm = ignoreOpeningConfirm;
  if (ignoreOpeningConfirm && !mappedInput.isPressed(MappedInputManager::Button::Confirm)) ignoreOpeningConfirm = false;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      (generating && (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                      mappedInput.isPressed(MappedInputManager::Button::Back)))) {
    if (generating) {
      RenderLock lock;
      batch.cancel();
      consumeBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
      generating = false;
      results = true;
      selected = 0;
      requestUpdate();
    } else if (detail || review || results) {
      RenderLock lock;
      if (results) selected = 0;
      detail = false;
      review = results = false;
      requestUpdate();
    } else if (returnToMenu) {
      finish();
    } else {
      onGoHome();
    }
    return;
  }
  if (!scanner.done()) {
    // Serialize model mutation with the render task. Storage never acquires this lock.
    RenderLock lock;
    HalPowerManager::Lock power;
    const bool complete = scanner.step();
    const unsigned long now = millis();
    if (complete || now - lastProgressRender >= 1000) {
      lastProgressRender = now;
      requestUpdate();
    }
    if (complete) {
      selection.assign(scanner.books().size(), 0);
      LOG_INF("SHARE", "Discovery: books=%u missing=%u legacy=%u warnings=%u EPUBs=%u ms=%lu heap=%u",
              static_cast<unsigned>(scanner.books().size()), static_cast<unsigned>(scanner.missingBooks),
              static_cast<unsigned>(scanner.legacyCount), static_cast<unsigned>(scanner.warnings),
              static_cast<unsigned>(scanner.checkedFiles), now - startedAt, ESP.getFreeHeap());
    }
    return;
  }
  if (generating) {
    RenderLock lock;
    HalPowerManager::Lock power;
    batch.step(scanner.books(), READING_HISTORY, [this] {
      const bool alreadyPressed = mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                                  mappedInput.isPressed(MappedInputManager::Button::Back);
      mappedInput.update();
      const bool cancel = alreadyPressed || mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                          mappedInput.isPressed(MappedInputManager::Button::Back);
      if (cancel) consumeBackRelease = true;
      return cancel;
    });
    if (batch.done()) {
      generating = false;
      results = true;
      selected = 0;
    }
    requestUpdate();
    return;
  }
  if (scanner.books().empty()) return;
  if (!ignoreConfirm && !results && !review && !detail && selected < scanner.books().size() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= 700) {
    RenderLock lock;
    detail = true;
    consumeConfirmRelease = true;
    requestUpdate();
    return;
  }
  if (!ignoreConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    RenderLock lock;
    if (consumeConfirmRelease) {
      consumeConfirmRelease = false;
      return;
    }
    if (review) {
      batch.start(selection);
      review = false;
      generating = true;
    } else if (!detail && !results) {
      if (selected < selection.size()) {
        selection[selected] = !selection[selected];
      } else if (std::count(selection.begin(), selection.end(), 1) > 0) {
        review = true;
      }
    }
    requestUpdate();
    return;
  }
  if (detail || review) return;
  const int count = static_cast<int>(results ? batch.results().size() : scanner.books().size() + 1);
  navigator.onNextRelease([this, count] {
    RenderLock lock;
    selected = ButtonNavigator::nextIndex(static_cast<int>(selected), count);
    requestUpdate();
  });
  navigator.onPreviousRelease([this, count] {
    RenderLock lock;
    selected = ButtonNavigator::previousIndex(static_cast<int>(selected), count);
    requestUpdate();
  });
  const auto layout = UiLayout::from(renderer);
  const int pageItems = std::max(1, UITheme::getNumberOfItemsPerPage(renderer, true, false, !layout.landscape, true,
                                                                     3 * (renderer.getLineHeight(UI_10_FONT_ID) + 8)));
  navigator.onNextContinuous([this, count, pageItems] {
    RenderLock lock;
    selected = ButtonNavigator::nextPageIndex(static_cast<int>(selected), count, pageItems);
    requestUpdate();
  });
  navigator.onPreviousContinuous([this, count, pageItems] {
    RenderLock lock;
    selected = ButtonNavigator::previousPageIndex(static_cast<int>(selected), count, pageItems);
    requestUpdate();
  });
}
std::string ShareCandidatesActivity::unitSummary(const ShareCandidate& book) const {
  std::string text;
  for (unsigned i = 0; i < 4; ++i) {
    if (!(book.units & Units[i])) continue;
    if (!text.empty()) text += " / ";
    text += I18n::getInstance().get(UnitLabels[i]);
    if (book.errors & Units[i])
      text += "!";
    else if (book.emptyUnits & Units[i])
      text += tr(STR_SHARE_EMPTY_RECORD);
  }
  return text;
}
void ShareCandidatesActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto layout = UiLayout::from(renderer);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool inverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int headerY = layout.content.y + metrics.topPadding + (inverted ? metrics.buttonHintsHeight : 0);
  const int bottom = layout.content.y + layout.content.height -
                     ((!layout.landscape && !inverted) ? metrics.buttonHintsHeight : 0) - metrics.verticalSpacing;
  const int centerOffset = layout.content.x + layout.content.width / 2 - renderer.getScreenWidth() / 2;
  const auto centered = [&](int y, const std::string& text) {
    const auto clipped = renderer.truncatedText(UI_10_FONT_ID, text.c_str(), layout.content.width - 20);
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, y, clipped.c_str(), true, centerOffset);
  };
  const auto header = std::string(tr(STR_SHARE_BOOKS)) + " (" + std::to_string(scanner.books().size()) + ")";
  GUI.drawHeader(renderer, Rect{layout.content.x, headerY, layout.content.width, metrics.headerHeight}, header.c_str());
  const int top = headerY + metrics.headerHeight + metrics.verticalSpacing;
  const int line = renderer.getLineHeight(UI_10_FONT_ID) + 8;
  if (!scanner.done()) {
    centered(top + line, tr(STR_SHARE_SEARCHING));
    centered(top + line * 2, std::to_string(scanner.checkedFiles) + " EPUB");
  } else if (review || generating) {
    centered(top + line, generating ? tr(STR_SHARE_GENERATING) : tr(STR_SHARE_EXPORT_CONFIRM));
    const auto count = static_cast<size_t>(std::count(selection.begin(), selection.end(), 1));
    centered(top + line * 2, generating ? std::to_string(batch.completed()) + " / " + std::to_string(count)
                                        : std::to_string(count) + " " + tr(STR_SHARE_BOOKS));
    centered(top + line * 3, "/YomukaSync/<BookId>.json");
    centered(top + line * 4, tr(STR_SHARE_REPLACE_NOTICE));
    centered(top + line * 5, tr(STR_SHARE_LOCAL_ONLY));
    if (generating) centered(top + line * 6, tr(STR_SHARE_CANCEL_HINT));
  } else if (results) {
    size_t saved = 0, failed = 0, cancelled = 0;
    for (const auto& entry : batch.results()) {
      saved += entry.result == BatchResult::Saved;
      failed += entry.result == BatchResult::Failed;
      cancelled += entry.result == BatchResult::Cancelled;
    }
    centered(top, std::string(tr(STR_SHARE_SUCCESS)) + ": " + std::to_string(saved) + " / " + tr(STR_SHARE_FAILED) +
                      ": " + std::to_string(failed) + " / " + tr(STR_SHARE_CANCELLED) + ": " +
                      std::to_string(cancelled));
    GUI.drawList(
        renderer, Rect{layout.content.x, top + line, layout.content.width, bottom - top - line},
        static_cast<int>(batch.results().size()), static_cast<int>(selected),
        [this](int i) { return scanner.books()[batch.results()[i].index].title; },
        [this](int i) {
          const auto& entry = batch.results()[i];
          if (entry.result == BatchResult::Saved) return std::string(tr(STR_SHARE_SUCCESS));
          if (entry.result == BatchResult::Cancelled) return std::string(tr(STR_SHARE_CANCELLED));
          return std::string(entry.error == BatchError::Source ? tr(STR_SHARE_SOURCE_ERROR)
                             : entry.error == BatchError::Data ? tr(STR_SHARE_DATA_ERROR)
                                                               : tr(STR_SHARE_WRITE_ERROR));
        });
  } else if (detail && selected < scanner.books().size()) {
    const auto& book = scanner.books()[selected];
    centered(top, book.title);
    centered(top + line, book.path);
    for (unsigned i = 0; i < 4; ++i) {
      std::string text = I18n::getInstance().get(UnitLabels[i]);
      text += ": ";
      text += I18n::getInstance().get((book.errors & Units[i])       ? StrId::STR_SHARE_NEEDS_CHECK
                                      : !(book.units & Units[i])     ? StrId::STR_SHARE_NOT_SAVED
                                      : (book.emptyUnits & Units[i]) ? StrId::STR_SHARE_CLEAR_RECORD
                                                                     : StrId::STR_SHARE_SAVED);
      centered(top + line * (i + 2), text);
    }
    centered(top + line * 6, selection[selected] ? tr(STR_SHARE_SELECTED) : tr(STR_SHARE_UNSELECTED));
  } else {
    centered(top, std::to_string(std::count(selection.begin(), selection.end(), 1)) + " / " +
                      std::to_string(scanner.books().size()) + " " + tr(STR_SHARE_SELECTED));
    const int footerLines = 3;
    const int listTop = top + line;
    const int height = bottom - listTop - line * footerLines;
    if (scanner.books().empty())
      centered(listTop + line, tr(STR_NO_BOOKS_FOUND));
    else if (height >= metrics.listWithSubtitleRowHeight) {
      GUI.drawList(
          renderer, Rect{layout.content.x, listTop, layout.content.width, height},
          static_cast<int>(scanner.books().size() + 1), static_cast<int>(selected),
          [this](int index) {
            if (static_cast<size_t>(index) == scanner.books().size()) return std::string(tr(STR_SHARE_EXPORT));
            return std::string(selection[index] ? "[x] " : "[ ] ") + scanner.books()[index].title;
          },
          [this](int index) {
            if (static_cast<size_t>(index) == scanner.books().size()) return std::string(tr(STR_SHARE_LOCAL_ONLY));
            const auto& book = scanner.books()[index];
            return unitSummary(book) +
                   (book.historyRank == UINT16_MAX ? " / " + std::string(tr(STR_SHARE_ORDER_UNKNOWN)) : "");
          });
    }
    centered(bottom - line * 3, tr(STR_SHARE_SELECT_HELP));
    centered(bottom - line * 2, std::string(tr(STR_SHARE_MISSING)) + ": " + std::to_string(scanner.missingBooks));
    centered(bottom - line, std::string(tr(STR_SHARE_NEEDS_CHECK)) + ": " + std::to_string(scanner.warnings) + " / " +
                                tr(STR_SHARE_LEGACY) + ": " + std::to_string(scanner.legacyCount));
  }
  const auto labels = mappedInput.mapLabels(
      tr(STR_BACK),
      review                                                                             ? tr(STR_SHARE_WRITE_SHORT)
      : scanner.done() && !detail && !generating && !results && !scanner.books().empty() ? tr(STR_SELECT)
                                                                                         : "",
      scanner.done() && !detail && !review && !generating ? tr(STR_DIR_UP) : "",
      scanner.done() && !detail && !review && !generating ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
