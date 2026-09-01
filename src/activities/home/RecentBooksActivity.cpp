#include "RecentBooksActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <FsHelpers.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "ReadingStatusHelper.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/CacheStatusIcon.h"
#include "fontIds.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr int CACHE_STATUS_ICON_RADIUS = 7;
constexpr char CACHE_STATUS_VALUE_SPACER[] = "    ";
}  // namespace

void RecentBooksActivity::loadRecentBooks() {
  recentBooks.clear();
  bookStatuses.clear();
  bookCacheStatuses.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(books.size());
  bookStatuses.reserve(books.size());
  bookCacheStatuses.reserve(books.size());

  for (const auto& book : books) {
    if (!Storage.exists(book.path.c_str())) {
      continue;
    }
    recentBooks.push_back(book);
    bookStatuses.push_back(getReadingStatus(book.path, "/.crosspoint"));
    bookCacheStatuses.push_back(FsHelpers::hasEpubExtension(book.path)
                                    ? Epub(book.path, "/.crosspoint").getCacheGenerationStatus()
                                    : Epub::CacheGenerationStatus::NotGenerated);
  }
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Load data
  loadRecentBooks();

  selectorIndex = 0;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  bookStatuses.clear();
  bookCacheStatuses.clear();
}

void RecentBooksActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && selectorIndex < static_cast<int>(recentBooks.size())) {
      LOG_DBG("RBA", "Selected recent book: %s", recentBooks[selectorIndex].path.c_str());
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
  }

  int listSize = static_cast<int>(recentBooks.size());

  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MENU_RECENT_BOOKS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // Recent tab
  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_RECENT_BOOKS));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, recentBooks.size(), selectorIndex,
        [this](int index) { return recentBooks[index].title; }, [this](int index) { return recentBooks[index].author; },
        [this](int index) { return UITheme::getFileIcon(recentBooks[index].path, bookStatuses[index]); },
        [this](int index) {
          return FsHelpers::hasEpubExtension(recentBooks[index].path) ? CACHE_STATUS_VALUE_SPACER : "";
        });

    const int rowHeight = metrics.listWithSubtitleRowHeight;
    const int pageItems = std::max(1, contentHeight / rowHeight);
    const int pageStart = (selectorIndex / pageItems) * pageItems;
    const int iconCenterX = pageWidth - metrics.contentSidePadding - CACHE_STATUS_ICON_RADIUS - 10;
    for (int index = pageStart; index < static_cast<int>(recentBooks.size()) && index < pageStart + pageItems; ++index) {
      if (!FsHelpers::hasEpubExtension(recentBooks[index].path)) continue;
      const int iconCenterY = contentTop + (index - pageStart) * rowHeight + rowHeight / 2;
      const bool ink = UITheme::getInstance().getTheme().showsFileIcons() || index != selectorIndex;
      CacheStatusIcon::draw(renderer, bookCacheStatuses[index], CACHE_STATUS_ICON_RADIUS, iconCenterX, iconCenterY, ink);
    }
  }

  // Help text
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
