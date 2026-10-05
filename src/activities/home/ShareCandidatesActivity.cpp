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
  // The transfer chooser opens this activity on press; consume that key's release.
  const bool ignoreConfirm = ignoreOpeningConfirm;
  if (ignoreOpeningConfirm && !mappedInput.isPressed(MappedInputManager::Button::Confirm)) ignoreOpeningConfirm = false;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (detail) {
      RenderLock lock;
      detail = false;
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
      LOG_INF("SHARE", "Discovery: books=%u missing=%u legacy=%u warnings=%u EPUBs=%u ms=%lu heap=%u",
              static_cast<unsigned>(scanner.books().size()), static_cast<unsigned>(scanner.missingBooks),
              static_cast<unsigned>(scanner.legacyCount), static_cast<unsigned>(scanner.warnings),
              static_cast<unsigned>(scanner.checkedFiles), now - startedAt, ESP.getFreeHeap());
    }
    return;
  }
  if (scanner.books().empty() || detail) return;
  if (!ignoreConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    RenderLock lock;
    detail = true;
    requestUpdate();
    return;
  }
  const int count = static_cast<int>(scanner.books().size());
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
    centered(top + line * 6, tr(STR_SHARE_LIST_ONLY));
  } else {
    centered(top, tr(STR_SHARE_ORDER));
    const int footerLines = 2;
    const int listTop = top + line;
    const int height = bottom - listTop - line * footerLines;
    if (scanner.books().empty())
      centered(listTop + line, tr(STR_NO_BOOKS_FOUND));
    else if (height >= metrics.listWithSubtitleRowHeight) {
      GUI.drawList(
          renderer, Rect{layout.content.x, listTop, layout.content.width, height},
          static_cast<int>(scanner.books().size()), static_cast<int>(selected),
          [this](int index) { return scanner.books()[index].title; },
          [this](int index) {
            const auto& book = scanner.books()[index];
            return unitSummary(book) +
                   (book.historyRank == UINT16_MAX ? " / " + std::string(tr(STR_SHARE_ORDER_UNKNOWN)) : "");
          });
    }
    centered(bottom - line * 2, std::string(tr(STR_SHARE_MISSING)) + ": " + std::to_string(scanner.missingBooks));
    centered(bottom - line, std::string(tr(STR_SHARE_NEEDS_CHECK)) + ": " + std::to_string(scanner.warnings) + " / " +
                                tr(STR_SHARE_LEGACY) + ": " + std::to_string(scanner.legacyCount));
  }
  const auto labels = mappedInput.mapLabels(
      tr(STR_BACK), scanner.done() && !detail && !scanner.books().empty() ? tr(STR_SELECT) : "",
      scanner.done() && !detail ? tr(STR_DIR_UP) : "", scanner.done() && !detail ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
