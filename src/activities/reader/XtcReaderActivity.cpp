/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <HalTiltSensor.h>
#include <I18n.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "MappedInputManager.h"
#include "ProgressFile.h"
#include "ReadingHistoryStore.h"
#include "RecentBooksStore.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "XtcReaderMenuActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace {
constexpr unsigned long skipPageMs = 700;
constexpr unsigned long goHomeMs = 1000;
}  // namespace

void XtcReaderActivity::onEnter() {
  Activity::onEnter();

  if (!xtc) {
    return;
  }

  xtc->setupCacheDir();

  // Load saved progress
  loadProgress();

  // Save current XTC as last opened book and add to recent books
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), xtc->getThumbBmpPath());
  READING_HISTORY.beginSession(xtc->getPath(), xtc->getTitle(), xtc->getAuthor());

  // Trigger first update
  requestUpdate();
}

void XtcReaderActivity::onExit() {
  Activity::onExit();
  READING_HISTORY.endSession();

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  xtc.reset();
}

void XtcReaderActivity::loop() {
  READING_HISTORY.tick();
  if (mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased()) READING_HISTORY.noteInteraction();
  // リーダーメニューを開く
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!xtc) return;
    const bool hasChapters = xtc->hasChapters() && !xtc->getChapters().empty();
    startActivityForResult(
        std::make_unique<XtcReaderMenuActivity>(renderer, mappedInput, xtc->getTitle(), currentPage,
                                                xtc->getPageCount(), hasChapters),
        [this](const ActivityResult& result) {
          if (result.isCancelled) return;
          const auto& menu = std::get<MenuResult>(result.data);
          const auto action = static_cast<XtcReaderMenuActivity::MenuAction>(menu.action);
          switch (action) {
            case XtcReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
              startActivityForResult(
                  std::make_unique<XtcReaderChapterSelectionActivity>(renderer, mappedInput, xtc, currentPage),
                  [this](const ActivityResult& chapterResult) {
                    if (!chapterResult.isCancelled) {
                      currentPage = std::get<PageResult>(chapterResult.data).page;
                    }
                  });
              break;
            }
            case XtcReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
              const int percent =
                  xtc->getPageCount() > 0
                      ? static_cast<int>(static_cast<float>(currentPage) / xtc->getPageCount() * 100.0f + 0.5f)
                      : 0;
              startActivityForResult(
                  std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, percent),
                  [this](const ActivityResult& percentResult) {
                    if (!percentResult.isCancelled) {
                      const int selectedPercent = std::get<PercentResult>(percentResult.data).percent;
                      currentPage =
                          static_cast<uint32_t>(static_cast<float>(selectedPercent) / 100.0f * xtc->getPageCount());
                      if (currentPage >= xtc->getPageCount() && xtc->getPageCount() > 0) {
                        currentPage = xtc->getPageCount() - 1;
                      }
                    }
                  });
              break;
            }
            case XtcReaderMenuActivity::MenuAction::FORCE_REFRESH: {
              // Re-render the current pre-rendered page through the normal
              // half-refresh path. This preserves position and XTH grayscale.
              pagesUntilFullRefresh = 1;
              requestUpdate();
              break;
            }
            case XtcReaderMenuActivity::MenuAction::SCREENSHOT: {
              pendingScreenshot = true;
              break;
            }
            case XtcReaderMenuActivity::MenuAction::GO_HOME: {
              onGoHome();
              return;
            }
            case XtcReaderMenuActivity::MenuAction::TILT_PAGE_TURN:
              break;
          }
        });
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    activityManager.goToFileBrowser(xtc ? xtc->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
    onGoHome();
    return;
  }

  // When long-press chapter skip is disabled, turn pages on press instead of release.
  const bool usePressForPageTurn = !SETTINGS.longPressChapterSkip;
  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  const bool prevTriggered =
      tiltPrev || (usePressForPageTurn ? (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
                                          mappedInput.wasPressed(MappedInputManager::Button::Left))
                                       : (mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                                          mappedInput.wasReleased(MappedInputManager::Button::Left)));
  const bool powerPageTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                             mappedInput.wasReleased(MappedInputManager::Button::Power);
  const bool nextTriggered =
      tiltNext || (usePressForPageTurn ? (mappedInput.wasPressed(MappedInputManager::Button::PageForward) ||
                                          powerPageTurn || mappedInput.wasPressed(MappedInputManager::Button::Right))
                                       : (mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                                          powerPageTurn || mappedInput.wasReleased(MappedInputManager::Button::Right)));

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentPage >= xtc->getPageCount()) {
    if (prevTriggered) {
      onGoHome();
    } else {
      currentPage = xtc->getPageCount() - 1;
      requestUpdate();
    }
    return;
  }

  const bool fromTilt = tiltPrev || tiltNext;
  const bool skipPages = !fromTilt && SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipPageMs;
  const int skipAmount = skipPages ? 10 : 1;

  if (prevTriggered) {
    currentPage += skipAmount;
    if (currentPage >= xtc->getPageCount()) {
      currentPage = xtc->getPageCount();  // Allow showing "End of book"
      saveProgress(true);
    }
    requestUpdate();
  } else if (nextTriggered) {
    if (currentPage >= static_cast<uint32_t>(skipAmount)) {
      currentPage -= skipAmount;
    } else {
      currentPage = 0;
    }
    requestUpdate();
  }
}

void XtcReaderActivity::render(RenderLock&&) {
  if (!xtc) {
    return;
  }

  // Bounds check
  if (currentPage >= xtc->getPageCount()) {
    // Show end of book screen
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderPage();
  {
    bool nearEnd = xtc && xtc->getPageCount() > 0 && static_cast<float>(currentPage + 1) / xtc->getPageCount() >= 0.95f;
    saveProgress(nearEnd);
  }
}

XtcReaderActivity::StatusBarInfo XtcReaderActivity::getStatusBarInfo() const {
  const int bookPageCount = static_cast<int>(xtc->getPageCount());
  const int bookPage = static_cast<int>(currentPage) + 1;
  std::string title =
      SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE ? xtc->getTitle() : "";
  if (!xtc->hasChapters()) return {bookPage, bookPageCount, std::move(title)};

  const auto& chapters = xtc->getChapters();
  const auto chapter = std::find_if(chapters.begin(), chapters.end(), [this](const xtc::ChapterInfo& item) {
    return currentPage >= item.startPage && currentPage <= item.endPage;
  });
  if (chapter == chapters.end() || chapter->endPage < chapter->startPage) {
    return {bookPage, bookPageCount, std::move(title)};
  }
  if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = chapter->name.empty() ? tr(STR_UNNAMED) : chapter->name;
  }
  return {static_cast<int>(currentPage - chapter->startPage) + 1,
          static_cast<int>(chapter->endPage - chapter->startPage) + 1, std::move(title)};
}

void XtcReaderActivity::renderStatusBarOverlay(const StatusBarOverlayPosition position) const {
  const bool drawBottom = SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_BOTTOM &&
                          position == StatusBarOverlayPosition::Bottom;
  const bool drawTop = SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_TOP &&
                       position == StatusBarOverlayPosition::Top;
  if ((!drawBottom && !drawTop) || !xtc || xtc->getPageCount() == 0) return;

  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight <= 0) return;

  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  const int screenHeight = renderer.getScreenHeight();
  int clearY = marginTop;
  int paddingBottom = 0;
  if (position == StatusBarOverlayPosition::Bottom) {
    clearY = std::max(0, screenHeight - marginBottom - statusBarHeight - 4);
  } else {
    paddingBottom = screenHeight - statusBarHeight - marginBottom - marginTop - 4;
  }
  const int clearHeight = position == StatusBarOverlayPosition::Bottom ? screenHeight - marginBottom - clearY
                                                                         : statusBarHeight + 4;
  renderer.fillRect(0, clearY, renderer.getScreenWidth(), clearHeight, false);

  const int pageCount = static_cast<int>(xtc->getPageCount());
  const float bookProgress = static_cast<float>(currentPage + 1) * 100.0f / pageCount;
  const auto pageInfo = getStatusBarInfo();
  GUI.drawStatusBar(renderer, bookProgress, pageInfo.currentPage, pageInfo.pageCount, pageInfo.title, paddingBottom,
                    0, true);
}

void XtcReaderActivity::renderPage() {
  // XTC pages are pre-rendered images; disable dark mode to preserve
  // original appearance (clearScreen fills white, pixels not inverted).
  const bool wasDarkMode = renderer.isDarkMode();
  renderer.setDarkMode(false);

  const uint16_t pageWidth = xtc->getPageWidth();
  const uint16_t pageHeight = xtc->getPageHeight();
  const uint8_t bitDepth = xtc->getBitDepth();

  // Calculate buffer size for one page
  // XTG (1-bit): Row-major, ((width+7)/8) * height bytes
  // XTH (2-bit): Two bit planes, column-major, ((width * height + 7) / 8) * 2 bytes
  size_t pageBufferSize;
  if (bitDepth == 2) {
    pageBufferSize = ((static_cast<size_t>(pageWidth) * pageHeight + 7) / 8) * 2;
  } else {
    pageBufferSize = ((pageWidth + 7) / 8) * pageHeight;
  }

  // Allocate page buffer
  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(pageBufferSize));
  if (!pageBuffer) {
    LOG_ERR("XTR", "Failed to allocate page buffer (%lu bytes)", pageBufferSize);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Load page data
  size_t bytesRead = xtc->loadPage(currentPage, pageBuffer, pageBufferSize);
  if (bytesRead == 0) {
    LOG_ERR("XTR", "Failed to load page %lu", currentPage);
    free(pageBuffer);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Clear screen first
  renderer.clearScreen();

  // Copy page bitmap using GfxRenderer's drawPixel
  // XTC/XTCH pages are pre-rendered with status bar included, so render full page
  const uint16_t maxSrcY = pageHeight;

  if (bitDepth == 2) {
    // XTH 2-bit mode: Two bit planes, column-major order
    // - Columns scanned right to left (x = width-1 down to 0)
    // - 8 vertical pixels per byte (MSB = topmost pixel in group)
    // - First plane: Bit1, Second plane: Bit2
    // - Pixel value = (bit1 << 1) | bit2
    // - Grayscale: 0=White, 1=Dark Grey, 2=Light Grey, 3=Black

    const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;
    const uint8_t* plane1 = pageBuffer;              // Bit1 plane
    const uint8_t* plane2 = pageBuffer + planeSize;  // Bit2 plane
    const size_t colBytes = (pageHeight + 7) / 8;    // Bytes per column (100 for 800 height)

    // Lambda to get pixel value at (x, y)
    auto getPixelValue = [&](uint16_t x, uint16_t y) -> uint8_t {
      const size_t colIndex = pageWidth - 1 - x;
      const size_t byteInCol = y / 8;
      const size_t bitInByte = 7 - (y % 8);
      const size_t byteOffset = colIndex * colBytes + byteInCol;
      const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
      const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
      return (bit1 << 1) | bit2;
    };

    // Optimized grayscale rendering without storeBwBuffer (saves 48KB peak memory)
    // Flow: BW display → LSB/MSB passes → grayscale display → re-render BW for next frame

    // Count pixel distribution for debugging
    uint32_t pixelCounts[4] = {0, 0, 0, 0};
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        pixelCounts[getPixelValue(x, y)]++;
      }
    }
    LOG_DBG("XTR", "Pixel distribution: White=%lu, DarkGrey=%lu, LightGrey=%lu, Black=%lu", pixelCounts[0],
            pixelCounts[1], pixelCounts[2], pixelCounts[3]);

    // Pass 1: BW buffer - draw all non-white pixels as black
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    // Display BW with conditional refresh based on pagesUntilFullRefresh
    if (pagesUntilFullRefresh <= 1) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      renderer.preconditionGrayscale();
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else {
      renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
      pagesUntilFullRefresh--;
    }

    // Pass 2: LSB buffer - mark DARK gray only (XTH value 1)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) == 1) {  // Dark grey only
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleLsbBuffers();

    // Pass 3: MSB buffer - mark LIGHT AND DARK gray (XTH value 1 or 2)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        const uint8_t pv = getPixelValue(x, y);
        if (pv == 1 || pv == 2) {  // Dark grey or Light grey
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleMsbBuffers();

    // Display grayscale overlay
    renderer.displayGrayBuffer();

    // Pass 4: Re-render BW to framebuffer (restore for next frame, instead of restoreBwBuffer)
    renderer.clearScreen();
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    // Cleanup grayscale buffers with current frame buffer
    renderer.cleanupGrayscaleWithFrameBuffer();

    free(pageBuffer);

    LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit grayscale)", currentPage + 1, xtc->getPageCount());
    renderer.setDarkMode(wasDarkMode);

    if (pendingScreenshot) {
      pendingScreenshot = false;
      ScreenshotUtil::takeScreenshot(renderer);
    }
    return;
  } else {
    // 1-bit mode: 8 pixels per byte, MSB first
    const size_t srcRowBytes = (pageWidth + 7) / 8;  // 60 bytes for 480 width

    for (uint16_t srcY = 0; srcY < maxSrcY; srcY++) {
      const size_t srcRowStart = srcY * srcRowBytes;

      for (uint16_t srcX = 0; srcX < pageWidth; srcX++) {
        // Read source pixel (MSB first, bit 7 = leftmost pixel)
        const size_t srcByte = srcRowStart + srcX / 8;
        const size_t srcBit = 7 - (srcX % 8);
        const bool isBlack = !((pageBuffer[srcByte] >> srcBit) & 1);  // XTC: 0 = black, 1 = white

        if (isBlack) {
          renderer.drawPixel(srcX, srcY, true);
        }
      }
    }
  }
  // White pixels are already cleared by clearScreen()

  free(pageBuffer);

  // A 1-bit XTC can safely accept this final framebuffer overlay. The 2-bit
  // branch above has already returned because it requires a separate grayscale
  // overlay for every render plane.
  if (SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_TOP) {
    renderStatusBarOverlay(StatusBarOverlayPosition::Top);
  } else {
    renderStatusBarOverlay(StatusBarOverlayPosition::Bottom);
  }

  // Display with appropriate refresh
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

  LOG_DBG("XTR", "Rendered page %lu/%lu (%u-bit)", currentPage + 1, xtc->getPageCount(), bitDepth);
  renderer.setDarkMode(wasDarkMode);

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}

void XtcReaderActivity::saveProgress(bool isFinished) const {
  uint8_t data[5];
  data[0] = currentPage & 0xFF;
  data[1] = (currentPage >> 8) & 0xFF;
  data[2] = (currentPage >> 16) & 0xFF;
  data[3] = (currentPage >> 24) & 0xFF;
  data[4] = isFinished ? 1 : 0;
  if (!ProgressFile::writeAtomic(xtc->getCachePath(), data, sizeof(data))) {
    LOG_ERR("XTR", "Could not save progress");
  }
}

void XtcReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[5] = {0};
    const int bytesRead = f.read(data, sizeof(data));
    if (bytesRead >= 4) {
      currentPage = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                    (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
      const bool isFinished = bytesRead >= 5 && data[4] == 1;
      LOG_DBG("XTR", "Loaded progress: page %lu, finished: %d", currentPage, isFinished ? 1 : 0);

      // pageCount is the valid end-screen position. Only values beyond it are corrupt.
      if (currentPage > xtc->getPageCount()) {
        currentPage = 0;
      }
    }
    f.close();
  }
}
