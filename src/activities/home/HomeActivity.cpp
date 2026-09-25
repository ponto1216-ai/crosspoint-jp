#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Utf8.h>
#include <Xtc.h>

#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReadingStatusHelper.h"
#include "RecentBooksStore.h"
#include "activities/settings/AozoraActivity.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"

int HomeActivity::getMenuItemCount() const {
  int count = 5;  // File Browser, Recents, Aozora, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  recentBookProgress.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));
  recentBookProgress.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (!Storage.exists(book.path.c_str())) {
      continue;
    }

    recentBooks.push_back(book);
    recentBookProgress.push_back(getReadingProgress(book.path, "/.crosspoint", book.bookId));
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  bool bufferLent = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading && !bufferLent) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          if (!bufferLent) GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          bool success;
          {
            GfxRenderer::FrameBufferLoan loan(renderer);
            success = epub.generateThumbBmp(coverHeight);
          }
          bufferLent = true;
          if (!success && !epub.hasCoverImage()) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          } else if (!success) {
            LOG_INF("HOME", "Thumbnail build failed; keeping cover path for retry: %s", book.path.c_str());
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading && !bufferLent) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            if (!bufferLent) GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success;
            {
              GfxRenderer::FrameBufferLoan loan(renderer);
              success = xtc.generateThumbBmp(coverHeight);
            }
            bufferLent = true;
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  selectorIndex = 0;

  loadRecentBooks(GUI.getHomeRecentBooksCount(renderer));

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // Store only the recent-cover area, not the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) {
    coverBufferStored = false;
    return false;
  }

  freeCoverBuffer();

  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", static_cast<unsigned>(needed));
    coverBufferSize = 0;
    coverBufferStored = false;
    return false;
  }

  coverBufferSize = needed;

  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    coverBufferStored = false;
    return false;
  }

  coverBufferStored = true;
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBufferStored || !coverBuffer || coverBufferSize == 0 || coverRectW <= 0 || coverRectH <= 0) {
    return false;
  }

  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }

  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Calculate dynamic indices based on which options are available
    int idx = 0;
    int menuSelectedIndex = selectorIndex - static_cast<int>(recentBooks.size());
    const int fileBrowserIdx = idx++;
    const int recentsIdx = idx++;
    const int aozoraIdx = idx++;
    const int fileTransferIdx = idx++;
    const int settingsIdx = idx;

    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
    } else if (menuSelectedIndex == fileBrowserIdx) {
      onFileBrowserOpen();
    } else if (menuSelectedIndex == recentsIdx) {
      onRecentsOpen();
    } else if (menuSelectedIndex == aozoraIdx) {
      onAozoraOpen();
    } else if (menuSelectedIndex == fileTransferIdx) {
      onFileTransferOpen();
    } else if (menuSelectedIndex == settingsIdx) {
      onSettingsOpen();
    }
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto layout = UiLayout::from(renderer);

  renderer.clearScreen();

  // Full render clears the screen, so do not restore old cover buffer here.
  // Force the recent cover area to be redrawn every time.
  coverRendered = false;
  coverBufferStored = false;
  bool bufferRestored = false;

  Rect recentRect;
  Rect menuRect;
  if (layout.landscape) {
    GUI.drawHeader(renderer, Rect{layout.content.x, metrics.topPadding, layout.content.width, metrics.homeTopPadding},
                   nullptr);
    const int contentTop = layout.content.y + metrics.homeTopPadding;
    const int contentHeight = layout.content.y + layout.content.height - contentTop - metrics.verticalSpacing;
    const int recentWidth = layout.content.width * GUI.getHomeLandscapeCoverPercent() / 100;
    const int recentHeight = std::min(
        contentHeight, GUI.getHomeCoverHeight(renderer) + metrics.homeCoverTileHeight - metrics.homeCoverHeight);
    const int menuHeight = 5 * (metrics.menuRowHeight + metrics.menuSpacing) + metrics.verticalSpacing;
    const int recentTop = contentTop + std::max(0, (contentHeight - recentHeight) / 4);
    const int menuTop = contentTop + std::max(0, (contentHeight - menuHeight) / 4) + GUI.getHomeLandscapeMenuOffset();
    recentRect = Rect{layout.content.x, recentTop, recentWidth, recentHeight};
    const int menuInset = GUI.getHomeLandscapeMenuInset();
    menuRect = Rect{layout.content.x + recentWidth - menuInset, menuTop, layout.content.width - recentWidth,
                    contentHeight - (menuTop - contentTop)};
  } else {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);
    recentRect = Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight};
    const int menuTop = GUI.getHomePortraitMenuTop(renderer);
    menuRect = Rect{0, menuTop, pageWidth,
                    pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing * 2 +
                                  metrics.buttonHintsHeight)};
  }

  const int coverCacheSidePadding = layout.landscape ? 0 : 60;
  coverRectX = recentRect.x + coverCacheSidePadding;
  coverRectY = recentRect.y;
  coverRectW = std::max(1, recentRect.width - coverCacheSidePadding * 2);
  coverRectH = recentRect.height;

  GUI.drawRecentBookCover(renderer, recentRect, recentBooks, recentBookProgress, selectorIndex, coverRendered,
                          coverBufferStored, bufferRestored, std::bind(&HomeActivity::storeCoverBuffer, this));

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};

  {
    menuItems.insert(menuItems.begin() + 2, tr(STR_AOZORA_BUNKO));
    menuIcons.insert(menuIcons.begin() + 2, Book);
  }

  GUI.drawButtonMenu(
      renderer, menuRect, static_cast<int>(menuItems.size()), selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // The first Home frame replaces an entire reader page.  Use the same
  // conditioned refresh path as the reader's one-page cadence so grayscale
  // text residue cannot survive the activity transition.
  // The second render only discovers missing cover thumbnails. The initial
  // HALF refresh already displayed this same frame, so avoid pushing it twice.
  const bool skipInitialFast = firstRenderDone && !recentsLoaded && !recentsLoading;
  if (!skipInitialFast) {
    renderer.displayBuffer(firstRenderDone ? HalDisplay::FAST_REFRESH : HalDisplay::HALF_REFRESH);
  }

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(GUI.getHomeCoverHeight(renderer));
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onAozoraOpen() {
  // カバーバッファと最近の本リストを解放（TLSバッファ用にヒープ確保）
  freeCoverBuffer();
  recentBooks.clear();
  recentBooks.shrink_to_fit();

  startActivityForResult(std::make_unique<AozoraActivity>(renderer, mappedInput), [this](const ActivityResult&) {
    // 戻ってきたら再読み込み（フラグリセットして描画を再トリガー）
    coverRendered = false;
    coverBufferStored = false;
    recentsLoaded = false;
    recentsLoading = false;
    loadRecentBooks(GUI.getHomeRecentBooksCount(renderer));
  });
}
