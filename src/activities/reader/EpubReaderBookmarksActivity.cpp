#include "EpubReaderBookmarksActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "OrientationHelper.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"
#include "util/BookDataPath.h"
#include "util/BookmarkUtil.h"

namespace {
// A 54px row places 12 two-line entries comfortably between the header and
// the delete hint on the portrait panel, without the large empty lower area.
constexpr int kLineHeight = 54;
constexpr int kDeleteHoldMs = 700;
constexpr int kMaximumRowsPerPage = 12;
}  // namespace

EpubReaderBookmarksActivity::EpubReaderBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                         const std::shared_ptr<Epub>& epub, const std::string& epubPath)
    : Activity("EpubReaderBookmarks", renderer, mappedInput),
      epub(epub),
      epubPath(epubPath),
      readerOrientation(renderer.getOrientation()) {}

void EpubReaderBookmarksActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(readerOrientation);
  mappedInput.setEffectiveOrientation(OrientationHelper::toInputOrientation(readerOrientation));
  if (epub) {
    const std::string legacyPath = BookmarkUtil::getBookmarkPath(epubPath);
    uint64_t bookId = 0;
    const bool hasBookId = epub->getSourceFingerprint(&bookId);
    const std::string path = hasBookId ? BookDataPath::getBookmarkPath(bookId) : legacyPath;
    BookmarkUtil::recoverBookmarkFile(path);
    if (Storage.exists(path.c_str())) {
      const String json = Storage.readFile(path.c_str());
      if (!json.isEmpty()) JsonSettingsIO::loadBookmarks(bookmarks, json.c_str(), MAX_BOOKMARKS);
    } else if (hasBookId) {
      BookmarkUtil::recoverBookmarkFile(legacyPath);
      if (Storage.exists(legacyPath.c_str())) {
        const String json = Storage.readFile(legacyPath.c_str());
        if (!json.isEmpty() && JsonSettingsIO::loadBookmarks(bookmarks, json.c_str(), MAX_BOOKMARKS) &&
            BookDataPath::ensureDirectory(bookId) && JsonSettingsIO::saveBookmarks(bookmarks, path.c_str())) {
          LOG_INF("BKM", "Migrated bookmarks to BookId %016llx", static_cast<unsigned long long>(bookId));
        }
      }
    }
  }
  requestUpdate();
}

void EpubReaderBookmarksActivity::save() {
  uint64_t bookId = 0;
  const bool hasBookId = epub && epub->getSourceFingerprint(&bookId);
  const std::string path = hasBookId ? BookDataPath::getBookmarkPath(bookId) : BookmarkUtil::getBookmarkPath(epubPath);
  if (!((!hasBookId || BookDataPath::ensureDirectory(bookId)) &&
        JsonSettingsIO::saveBookmarks(bookmarks, path.c_str()))) {
    LOG_ERR("BKM", "Failed to save bookmarks");
  }
}

void EpubReaderBookmarksActivity::loop() {
  if (bookmarks.empty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }

  if (deleteMode != DeleteMode::NONE) {
    if (ignoreDeleteOpeningRelease) {
      if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
          !mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        ignoreDeleteOpeningRelease = false;
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (deleteMode == DeleteMode::ALL) {
        bookmarks.clear();
      } else {
        bookmarks.erase(bookmarks.begin() + selectedIndex);
        if (selectedIndex >= static_cast<int>(bookmarks.size()) && selectedIndex > 0) --selectedIndex;
      }
      save();
      deleteMode = DeleteMode::NONE;
      if (bookmarks.empty()) {
        ActivityResult result;
        result.isCancelled = true;
        setResult(std::move(result));
        finish();
        return;
      }
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      deleteMode = DeleteMode::NONE;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= kDeleteHoldMs) {
    if (selectedIndex < static_cast<int>(bookmarks.size())) deleteMode = DeleteMode::ONE;
    ignoreDeleteOpeningRelease = true;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex == static_cast<int>(bookmarks.size())) {
      deleteMode = DeleteMode::ALL;
      requestUpdate();
      return;
    }
    const auto& bookmark = bookmarks.at(selectedIndex);
    setResult(
        BookmarkResult{bookmark.spineIndex, bookmark.chapterPage, bookmark.chapterPageCount, bookmark.percentage});
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, bookmarks.size() + 1);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, bookmarks.size() + 1);
    requestUpdate();
  });
}

void EpubReaderBookmarksActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto layout = UiLayout::from(renderer);
  const bool portraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int topHintGutter = portraitInverted ? metrics.buttonHintsHeight + metrics.verticalSpacing : 0;
  const int titleY = layout.content.y + 15 + topHintGutter;
  const int listTop = layout.content.y + 55 + topHintGutter;
  const int bottomHints = layout.landscape ? 0 : metrics.buttonHintsHeight + metrics.verticalSpacing;
  const int deleteHintHeight = renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
  const int listBottom = layout.content.y + layout.content.height - bottomHints - deleteHintHeight;
  const int rows = std::max(1, std::min(kMaximumRowsPerPage, (listBottom - listTop) / kLineHeight));
  const int itemCount = static_cast<int>(bookmarks.size()) + 1;  // final item is Delete All
  const int pageCount = std::max(1, (itemCount + rows - 1) / rows);
  const int currentListPage = bookmarks.empty() ? 1 : (selectedIndex / rows) + 1;
  const std::string title =
      std::string(tr(STR_BOOKMARKS)) + " " + std::to_string(currentListPage) + "/" + std::to_string(pageCount);
  const int centerOffset = layout.content.x + layout.content.width / 2 - renderer.getScreenWidth() / 2;
  renderer.drawCenteredTextOffset(UI_12_FONT_ID, titleY, title.c_str(), true, centerOffset, EpdFontFamily::BOLD);

  if (bookmarks.empty()) {
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, layout.content.y + layout.content.height / 2, tr(STR_NO_FILES_FOUND),
                                    true, centerOffset);
  } else if (deleteMode != DeleteMode::NONE) {
    const bool deleteAll = deleteMode == DeleteMode::ALL;
    const int centerY = layout.content.y + layout.content.height / 2;
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, centerY - kLineHeight,
                                    deleteAll ? tr(STR_CONFIRM_DELETE_ALL_BOOKMARKS) : tr(STR_CONFIRM_DELETE_BOOKMARK),
                                    true, centerOffset);
    if (!deleteAll)
      renderer.drawCenteredTextOffset(UI_10_FONT_ID, centerY, bookmarks.at(selectedIndex).summary.c_str(), true,
                                      centerOffset);
  } else {
    const int first = (selectedIndex / rows) * rows;
    for (int i = 0; i < rows && first + i < itemCount; ++i) {
      const int index = first + i;
      const bool selected = index == selectedIndex;
      const int y = listTop + i * kLineHeight;
      if (selected) renderer.fillRect(layout.content.x, y - 3, layout.content.width - 1, kLineHeight, true);
      if (index == static_cast<int>(bookmarks.size())) {
        renderer.drawText(UI_10_FONT_ID, layout.content.x + 15, y + 10, tr(STR_DELETE_ALL_BOOKMARKS), !selected);
        continue;
      }
      const auto& bookmark = bookmarks[index];
      const std::string title =
          renderer.truncatedText(UI_10_FONT_ID, bookmark.summary.c_str(), layout.content.width - 30);
      const std::string detail = std::to_string(static_cast<int>(bookmark.percentage * 100.0f + 0.5f)) + "%  " +
                                 std::to_string(bookmark.chapterPage + 1) + "/" +
                                 std::to_string(bookmark.chapterPageCount);
      renderer.drawText(UI_10_FONT_ID, layout.content.x + 15, y, title.c_str(), !selected);
      renderer.drawText(UI_10_FONT_ID, layout.content.x + 15, y + 22, detail.c_str(), !selected);
    }
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, listBottom + metrics.verticalSpacing / 2,
                                    tr(STR_HOLD_OPEN_TO_DELETE), true, centerOffset);
  }
  const auto labels = mappedInput.mapLabels(
      tr(STR_BACK), deleteMode != DeleteMode::NONE ? tr(STR_DELETE) : tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
