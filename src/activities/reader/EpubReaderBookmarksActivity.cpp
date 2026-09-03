#include "EpubReaderBookmarksActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/BookDataPath.h"

namespace {
// A 54px row places 12 two-line entries comfortably between the header and
// the delete hint on the portrait panel, without the large empty lower area.
constexpr int kLineHeight = 54;
constexpr int kDeleteHoldMs = 700;
constexpr int kMaximumRowsPerPage = 12;
}  // namespace

EpubReaderBookmarksActivity::EpubReaderBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                         const std::shared_ptr<Epub>& epub, const std::string& epubPath)
    : Activity("EpubReaderBookmarks", renderer, mappedInput), epub(epub), epubPath(epubPath) {}

void EpubReaderBookmarksActivity::onEnter() {
  Activity::onEnter();
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
  if (!((!hasBookId || BookDataPath::ensureDirectory(bookId)) && JsonSettingsIO::saveBookmarks(bookmarks, path.c_str()))) {
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
    setResult(BookmarkResult{bookmark.spineIndex, bookmark.chapterPage, bookmark.chapterPageCount, bookmark.percentage});
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
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const int footerHeight = UITheme::getInstance().getMetrics().buttonHintsHeight;
  const int rows = std::max(1, std::min(kMaximumRowsPerPage, (height - 55 - footerHeight - 30) / kLineHeight));
  const int itemCount = static_cast<int>(bookmarks.size()) + 1;  // final item is Delete All
  const int pageCount = std::max(1, (itemCount + rows - 1) / rows);
  const int currentListPage = bookmarks.empty() ? 1 : (selectedIndex / rows) + 1;
  const std::string title = std::string(tr(STR_BOOKMARKS)) + " " + std::to_string(currentListPage) + "/" +
                            std::to_string(pageCount);
  const int titleWidth = renderer.getTextWidth(UI_12_FONT_ID, title.c_str(), EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, (width - titleWidth) / 2, 15, title.c_str(), true, EpdFontFamily::BOLD);

  if (bookmarks.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, height / 2, tr(STR_NO_FILES_FOUND));
  } else if (deleteMode != DeleteMode::NONE) {
    const bool deleteAll = deleteMode == DeleteMode::ALL;
    renderer.drawCenteredText(UI_10_FONT_ID, height / 2 - kLineHeight,
                              deleteAll ? tr(STR_CONFIRM_DELETE_ALL_BOOKMARKS) : tr(STR_CONFIRM_DELETE_BOOKMARK));
    if (!deleteAll) renderer.drawCenteredText(UI_10_FONT_ID, height / 2, bookmarks.at(selectedIndex).summary.c_str());
  } else {
    const int first = (selectedIndex / rows) * rows;
    for (int i = 0; i < rows && first + i < itemCount; ++i) {
      const int index = first + i;
      const bool selected = index == selectedIndex;
      const int y = 55 + i * kLineHeight;
      if (selected) renderer.fillRect(0, y - 3, width - 1, kLineHeight, true);
      if (index == static_cast<int>(bookmarks.size())) {
        renderer.drawText(UI_10_FONT_ID, 15, y + 10, tr(STR_DELETE_ALL_BOOKMARKS), !selected);
        continue;
      }
      const auto& bookmark = bookmarks[index];
      const std::string title = renderer.truncatedText(UI_10_FONT_ID, bookmark.summary.c_str(), width - 30);
      const std::string detail = std::to_string(static_cast<int>(bookmark.percentage * 100.0f + 0.5f)) + "%  " +
                                 std::to_string(bookmark.chapterPage + 1) + "/" +
                                 std::to_string(bookmark.chapterPageCount);
      renderer.drawText(UI_10_FONT_ID, 15, y, title.c_str(), !selected);
      renderer.drawText(UI_10_FONT_ID, 15, y + 22, detail.c_str(), !selected);
    }
    renderer.drawCenteredText(UI_10_FONT_ID, height - footerHeight - 25, tr(STR_HOLD_OPEN_TO_DELETE));
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), deleteMode != DeleteMode::NONE ? tr(STR_DELETE) : tr(STR_SELECT),
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
