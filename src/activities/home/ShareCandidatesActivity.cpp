#include "ShareCandidatesActivity.h"

#include <HalPowerManager.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "ReceivedReviewActivity.h"
#include "activities/network/DirectShareActivity.h"
#include "components/ActionListDivider.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"
#include "sync/ShareSearchExclusions.h"

using namespace yomuka::sync;
namespace {
constexpr uint8_t Units[] = {SavedPosition, SavedBookmarks, SavedSettings, SavedHistory};
constexpr StrId UnitLabels[] = {StrId::STR_SHARE_POSITION, StrId::STR_SHARE_BOOKMARKS, StrId::STR_SHARE_SETTINGS,
                                StrId::STR_SHARE_HISTORY};
constexpr unsigned long ProgressRenderIntervalMs = 3000;
}  // namespace
void ShareCandidatesActivity::onEnter() {
  Activity::onEnter();
  exclusionsLoaded = ShareSearchExclusions::load(excludedIds);
  if (exclusionsLoaded)
    scanner.setExclusions(excludedIds);
  else
    LOG_ERR("SHARE", "Could not load search exclusions");
  startedAt = millis();
  lastProgressRender = startedAt;
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
    } else if (managingMissing) {
      RenderLock lock;
      managingMissing = false;
      selected = 0;
      actionMessage.clear();
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
    if (complete || now - lastProgressRender >= ProgressRenderIntervalMs) {
      lastProgressRender = now;
      requestUpdate();
    }
    if (complete) {
      selection.assign(scanner.books().size(), 0);
      if (fullSearchCompleted && scanner.missingBooks && exclusionsLoaded) {
        managingMissing = true;
        selected = 0;
      }
      LOG_INF("SHARE", "Discovery: books=%u missing=%u ignored=%u legacy=%u warnings=%u EPUBs=%u ms=%lu heap=%u",
              static_cast<unsigned>(scanner.books().size()), static_cast<unsigned>(scanner.missingBooks),
              static_cast<unsigned>(excludedIds.size()), static_cast<unsigned>(scanner.legacyCount),
              static_cast<unsigned>(scanner.warnings), static_cast<unsigned>(scanner.checkedFiles), now - startedAt,
              ESP.getFreeHeap());
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
    if (managingMissing) {
      const auto indices = managedIndices();
      if (selected < indices.size() && exclusionsLoaded) {
        const auto id = scanner.unresolvedCandidates()[indices[selected]].bookId;
        auto updated = excludedIds;
        const auto it = std::find(updated.begin(), updated.end(), id);
        if (it == updated.end())
          updated.push_back(id);
        else
          updated.erase(it);
        if (ShareSearchExclusions::save(updated)) {
          excludedIds = std::move(updated);
          scanner.setExclusions(excludedIds);
          const auto next = managedIndices();
          if (selected >= next.size() && !next.empty()) selected = next.size() - 1;
          actionMessage.clear();
        } else {
          actionMessage = tr(STR_SHARE_EXCLUSIONS_SAVE_FAILED);
        }
      }
    } else if (review) {
      batch.start(selection);
      review = false;
      generating = true;
    } else if (!detail && !results) {
      if (selected < selection.size()) {
        selection[selected] = !selection[selected];
        actionMessage.clear();
      } else if (selected == selection.size() && std::count(selection.begin(), selection.end(), 1) > 0) {
        review = true;
      } else if (selected == selection.size() + 1) {
        if (std::count(selection.begin(), selection.end(), 1) == 0) {
          actionMessage = tr(STR_DIRECT_ONE_BOOK);
        } else {
          std::vector<yomuka::sync::ShareCandidate> books;
          for (size_t index = 0; index < selection.size(); ++index)
            if (selection[index]) books.push_back(scanner.books()[index]);
          startActivityForResult(std::make_unique<DirectShareActivity>(renderer, mappedInput, false, books),
                                 [this](const ActivityResult&) { ignoreOpeningConfirm = true; });
        }
      } else if (selected == selection.size() + 2) {
        startActivityForResult(
            std::make_unique<DirectShareActivity>(renderer, mappedInput, true), [this](const ActivityResult& result) {
              ignoreOpeningConfirm = true;
              if (const auto* received = std::get_if<ReceivedFilesResult>(&result.data);
                  received && !received->files.empty())
                startActivityForResult(std::make_unique<ReceivedReviewActivity>(renderer, mappedInput, received->files),
                                       [this](const ActivityResult&) { ignoreOpeningConfirm = true; });
            });
      } else if (selected == selection.size() + 3) {
        startActivityForResult(std::make_unique<ReceivedReviewActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) { ignoreOpeningConfirm = true; });
      } else if (selected == selection.size() + 4 && !excludedIds.empty()) {
        managingMissing = true;
        selected = 0;
        actionMessage.clear();
      } else if (selected == selection.size() + 4 + (!excludedIds.empty() ? 1 : 0) && scanner.canSearchMissing()) {
        scanner.restart(true);
        fullSearchCompleted = true;
        selection.clear();
        selected = 0;
        actionMessage.clear();
        startedAt = millis();
        lastProgressRender = startedAt;
      }
    }
    requestUpdate();
    return;
  }
  if (detail || review) return;
  const int count = static_cast<int>(managingMissing ? managedIndices().size()
                                     : results       ? batch.results().size()
                                                     : scanner.books().size() + 4 + (!excludedIds.empty() ? 1 : 0) +
                                                     (scanner.canSearchMissing() ? 1 : 0));
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
std::vector<size_t> ShareCandidatesActivity::managedIndices() const {
  std::vector<size_t> indices;
  const auto& candidates = scanner.unresolvedCandidates();
  for (size_t i = 0; i < candidates.size(); ++i)
    if (!candidates[i].deletionOnly() && (fullSearchCompleted || scanner.isExcluded(candidates[i].bookId)))
      indices.push_back(i);
  return indices;
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
  const auto header = managingMissing
                          ? std::string(tr(STR_SHARE_MISSING))
                          : std::string(tr(STR_SHARE_BOOKS)) + " (" + std::to_string(scanner.books().size()) + ")";
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
  } else if (managingMissing) {
    const auto indices = managedIndices();
    centered(top, actionMessage.empty()
                      ? std::string(tr(STR_SHARE_MISSING)) + ": " + std::to_string(scanner.missingBooks) + " / " +
                            tr(STR_SHARE_EXCLUDED) + ": " + std::to_string(excludedIds.size())
                      : actionMessage);
    GUI.drawList(
        renderer, Rect{layout.content.x, top + line, layout.content.width, bottom - top - line * 4},
        static_cast<int>(indices.size()), static_cast<int>(selected),
        [this, &indices](int i) {
          const auto& item = scanner.unresolvedCandidates()[indices[i]];
          if (!item.title.empty()) return item.title;
          if (!item.path.empty()) return item.path;
          char key[17];
          snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(item.bookId));
          return std::string(key);
        },
        [this, &indices](int i) {
          const bool ignored = scanner.isExcluded(scanner.unresolvedCandidates()[indices[i]].bookId);
          return std::string("　") +
                 I18n::getInstance().get(ignored ? StrId::STR_SHARE_IGNORED : StrId::STR_SHARE_WILL_SEARCH);
        });
    centered(bottom - line * 2, tr(STR_SHARE_EXCLUSION_HELP));
    centered(bottom - line, tr(STR_SHARE_EXCLUSION_LOCAL));
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
    centered(top, actionMessage.empty() ? std::to_string(std::count(selection.begin(), selection.end(), 1)) + " / " +
                                              std::to_string(scanner.books().size()) + " " + tr(STR_SHARE_SELECTED)
                                        : actionMessage);
    const int footerLines = 2;
    const int listTop = top + line;
    const int height = bottom - listTop - line * footerLines;
    if (height >= metrics.listWithSubtitleRowHeight) {
      const Rect listRect{layout.content.x, listTop, layout.content.width, height};
      GUI.drawList(
          renderer, listRect,
          static_cast<int>(scanner.books().size() + 4 + (!excludedIds.empty() ? 1 : 0) +
                           (scanner.canSearchMissing() ? 1 : 0)),
          static_cast<int>(selected),
          [this](int index) {
            if (static_cast<size_t>(index) == scanner.books().size()) return std::string(tr(STR_SHARE_EXPORT));
            if (static_cast<size_t>(index) == scanner.books().size() + 1) return std::string(tr(STR_DIRECT_SEND));
            if (static_cast<size_t>(index) == scanner.books().size() + 2) return std::string(tr(STR_DIRECT_RECEIVE));
            if (static_cast<size_t>(index) == scanner.books().size() + 3) return std::string(tr(STR_RECEIVED_TITLE));
            if (static_cast<size_t>(index) == scanner.books().size() + 4 && !excludedIds.empty())
              return std::string(tr(STR_SHARE_REVIEW_EXCLUDED));
            if (static_cast<size_t>(index) == scanner.books().size() + 4 + (!excludedIds.empty() ? 1 : 0))
              return std::string(tr(STR_SHARE_SEARCH_SD));
            return std::string(selection[index] ? "[x] " : "[ ] ") + scanner.books()[index].title;
          },
          [this](int index) {
            if (static_cast<size_t>(index) == scanner.books().size())
              return std::string("　") + tr(STR_SHARE_LOCAL_ONLY_SHORT);
            if (static_cast<size_t>(index) == scanner.books().size() + 3)
              return std::string("　") + tr(STR_RECEIVED_OFFLINE);
            if (static_cast<size_t>(index) == scanner.books().size() + 1)
              return std::string("　") + tr(STR_DIRECT_SEND_DESC);
            if (static_cast<size_t>(index) == scanner.books().size() + 2)
              return std::string("　") + tr(STR_DIRECT_RECEIVE_DESC);
            if (static_cast<size_t>(index) == scanner.books().size() + 4 && !excludedIds.empty())
              return std::string("　") + tr(STR_SHARE_REVIEW_EXCLUDED_DESC);
            if (static_cast<size_t>(index) == scanner.books().size() + 4 + (!excludedIds.empty() ? 1 : 0))
              return std::string("　") + tr(STR_SHARE_SEARCH_SD_DESC);
            const auto& book = scanner.books()[index];
            return std::string("　") + unitSummary(book) +
                   (book.historyRank == UINT16_MAX ? " / " + std::string(tr(STR_SHARE_ORDER_UNKNOWN)) : "");
          });
      drawActionListDivider(renderer, listRect, metrics.listWithSubtitleRowHeight, scanner.books().size(), selected);
    }
    centered(bottom - line * 2, exclusionsLoaded ? tr(STR_SHARE_SELECT_HELP) : tr(STR_SHARE_EXCLUSIONS_LOAD_FAILED));
    centered(bottom - line, std::string(tr(STR_SHARE_MISSING_SHORT)) + std::to_string(scanner.missingBooks) + " / " +
                                tr(STR_SHARE_NEEDS_CHECK_SHORT) + std::to_string(scanner.warnings) + " / " +
                                tr(STR_SHARE_LEGACY_SHORT) + std::to_string(scanner.legacyCount));
  }
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK),
                            review                                                 ? tr(STR_SHARE_WRITE_SHORT)
                            : scanner.done() && !detail && !generating && !results ? tr(STR_SELECT)
                                                                                   : "",
                            scanner.done() && !detail && !review && !generating ? tr(STR_DIR_UP) : "",
                            scanner.done() && !detail && !review && !generating ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
