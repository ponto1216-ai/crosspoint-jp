#include "TxtReaderActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Utf8.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ProgressFile.h"
#include "ReadingHistoryStore.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontGlobals.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 6;          // Horizontal-only TXT layout

struct TextLayoutMap {
  std::vector<size_t> boundaries;
  std::vector<int> extents;
};

TextLayoutMap getHorizontalLayout(const std::string& text, const int fullWidth) {
  TextLayoutMap result;
  result.boundaries.reserve(text.size() + 1);
  result.extents.reserve(text.size() + 1);
  result.boundaries.push_back(0);
  result.extents.push_back(0);

  const auto* begin = reinterpret_cast<const unsigned char*>(text.c_str());
  const unsigned char* ptr = begin;
  while (*ptr) {
    const uint32_t codepoint = utf8NextCodepoint(&ptr);
    int advance = 0;
    if (!utf8IsCombiningMark(codepoint)) {
      if (codepoint < 0x80) {
        advance = codepoint == ' ' ? std::max(1, fullWidth / 3) : std::max(1, fullWidth / 2);
      } else {
        advance = fullWidth;
      }
    }
    result.boundaries.push_back(static_cast<size_t>(ptr - begin));
    result.extents.push_back(result.extents.back() + advance);
  }
  return result;
}
}  // namespace

void TxtReaderActivity::onEnter() {
  Activity::onEnter();

  if (!txt) {
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  txt->setupCacheDir();

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");
  READING_HISTORY.beginSession(filePath, fileName, "");

  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  Activity::onExit();
  READING_HISTORY.endSession();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  pageOffsets.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

void TxtReaderActivity::loop() {
  READING_HISTORY.tick();
  if (mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased()) READING_HISTORY.noteInteraction();
  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(txt ? txt->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (prevTriggered && currentPage > 0) {
    --currentPage;
    requestUpdate();
  } else if (nextTriggered) {
    if (currentPage < totalPages - 1) {
      ++currentPage;
      requestUpdate();
    } else {
      saveProgress(true);
      onGoHome();
    }
  }
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Plain TXT intentionally uses one predictable layout regardless of the
  // global EPUB writing-mode setting.
  ensureSdFontLoaded(false);
  const auto& directionSettings = SETTINGS.getDirectionSettings(false);
  cachedFontId = SETTINGS.getReaderFontId(false);
  cachedScreenMargin = directionSettings.screenMargin;
  cachedParagraphAlignment = directionSettings.paragraphAlignment;
  renderer.setVerticalCharSpacing(0);

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int baseLineHeight = renderer.getLineHeight(cachedFontId);
  layoutGlyphAdvance = std::max(1, baseLineHeight);
  const int lineHeight =
      std::max(1, static_cast<int>(baseLineHeight * SETTINGS.getReaderLineCompression(false) + 0.5f));

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_INF("TRS", "Viewport: %dx%d, lines per page: %d, horizontal=1, font=%d, sdFont=%d", viewportWidth,
          viewportHeight, linesPerPage, cachedFontId,
          renderer.isSdCardFont(cachedFontId) ? 1 : 0);

  // Try to load cached page index first
  if (!loadPageIndexCache()) {
    // Cache not found, build page index
    buildPageIndex();
    // Save to cache for next time
    savePageIndexCache();
  }

  // Load saved progress
  loadProgress();

  initialized = true;
}

void TxtReaderActivity::buildPageIndex() {
  const unsigned long startedAt = millis();
  pageOffsets.clear();
  pageOffsets.push_back(txt->getContentOffset());

  size_t offset = txt->getContentOffset();
  const size_t fileSize = txt->getFileSize();

  LOG_DBG("TRS", "Building page index for %zu bytes...", fileSize);

  GUI.drawPopup(renderer, tr(STR_INDEXING));

  FsFile inputFile;
  if (!Storage.openFileForRead("TRS", txt->getPath(), inputFile)) {
    LOG_ERR("TRS", "Failed to open TXT for page indexing");
    totalPages = pageOffsets.size();
    return;
  }

  while (offset < fileSize) {
    std::vector<std::string> tempLines;
    size_t nextOffset = offset;

    if (!loadPageAtOffset(offset, tempLines, nextOffset, &inputFile)) {
      break;
    }

    if (nextOffset <= offset) {
      // No progress made, avoid infinite loop
      break;
    }

    offset = nextOffset;
    if (offset < fileSize) {
      pageOffsets.push_back(offset);
    }

    // Yield to other tasks periodically
    if (pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }
  inputFile.close();

  totalPages = pageOffsets.size();
  LOG_INF("TRS", "Built page index: pages=%d bytes=%zu duration=%lu ms", totalPages, fileSize,
          millis() - startedAt);
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset,
                                         FsFile* inputFile) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();
  if (offset >= fileSize) {
    return false;
  }
  nextOffset = offset;
  const int wrapExtent = viewportWidth;

  while (nextOffset < fileSize && static_cast<int>(outLines.size()) < linesPerPage) {
    Txt::DecodedLine decoded;
    const bool decodedOk = inputFile ? txt->readDecodedLine(*inputFile, nextOffset, CHUNK_SIZE, decoded)
                                     : txt->readDecodedLine(nextOffset, CHUNK_SIZE, decoded);
    if (!decodedOk) break;

    const bool forcePageBreak = decoded.pageBreak;
    if (forcePageBreak && decoded.text.empty()) {
      nextOffset = decoded.nextOffset;
      if (!outLines.empty()) break;
      continue;
    }

    if (decoded.text.empty()) {
      if (decoded.newline) outLines.emplace_back();
      nextOffset = decoded.nextOffset;
      continue;
    }

    const auto layout = getHorizontalLayout(decoded.text, layoutGlyphAdvance);
    const auto& boundaries = layout.boundaries;
    size_t textPos = 0;
    while (textPos < decoded.text.size() && static_cast<int>(outLines.size()) < linesPerPage) {
      const auto startIt = std::lower_bound(boundaries.begin(), boundaries.end(), textPos);
      const size_t startIndex = static_cast<size_t>(startIt - boundaries.begin());
      // Find the largest complete UTF-8 character range that fits. This keeps
      // indexing independent of SD-card glyph reads. TXT layout intentionally
      // uses full-width Japanese and half-width ASCII estimates.
      size_t low = startIndex + 1;
      size_t high = boundaries.size() - 1;
      size_t fittingIndex = startIndex;
      while (low <= high) {
        const size_t mid = low + (high - low) / 2;
        if (layout.extents[mid] - layout.extents[startIndex] <= wrapExtent) {
          fittingIndex = mid;
          low = mid + 1;
        } else {
          high = mid - 1;
        }
      }

      size_t breakPos = boundaries[fittingIndex > startIndex ? fittingIndex : startIndex + 1];
      if (breakPos < decoded.text.size()) {
        const size_t space = decoded.text.rfind(' ', breakPos - 1);
        if (space != std::string::npos && space > textPos) {
          breakPos = space;
        }
      }

      outLines.push_back(decoded.text.substr(textPos, breakPos - textPos));
      size_t consumedEnd = breakPos;
      if (consumedEnd < decoded.text.size() && decoded.text[consumedEnd] == ' ') ++consumedEnd;
      nextOffset = decoded.rawEnds[consumedEnd - 1];
      textPos = consumedEnd;
    }

    if (textPos >= decoded.text.size()) nextOffset = decoded.nextOffset;
    if (forcePageBreak) break;
  }

  return !outLines.empty() || nextOffset > offset;
}

void TxtReaderActivity::render(RenderLock&&) {
  if (!txt) {
    return;
  }

  // Initialize reader if not done
  if (!initialized) {
    initializeReader();
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // Load current page content
  size_t offset = pageOffsets[currentPage];
  size_t nextOffset;
  currentPageLines.clear();
  loadPageAtOffset(offset, currentPageLines, nextOffset);

  renderer.clearScreen();
  renderPage();

  // Save progress
  const bool nearEnd = totalPages > 0 && static_cast<float>(currentPage + 1) / totalPages >= 0.95f;
  saveProgress(nearEnd);

}

void TxtReaderActivity::renderPage() {
  const int baseLineHeight = renderer.getLineHeight(cachedFontId);
  const int lineHeight =
      std::max(1, static_cast<int>(baseLineHeight * SETTINGS.getReaderLineCompression(false) + 0.5f));
  const int contentWidth = viewportWidth;

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : currentPageLines) {
      if (!line.empty()) {
        int x = cachedOrientedMarginLeft;

        // Apply text alignment
        switch (cachedParagraphAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            // x already set to left margin
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            const int textWidth = getHorizontalLayout(line, layoutGlyphAdvance).extents.back();
            x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            const int textWidth = getHorizontalLayout(line, layoutGlyphAdvance).extents.back();
            x = cachedOrientedMarginLeft + contentWidth - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            // For plain text, justified is treated as left-aligned
            // (true justification would require word spacing adjustments)
            break;
        }

        renderer.drawText(cachedFontId, x, y, line.c_str());
      }
      y += lineHeight;
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();  // scan pass — text accumulated, no drawing
  renderStatusBar();
  scope.endScanAndPrewarm();

  // BW rendering
  renderLines();
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  // scope destructor clears font cache via FontCacheManager
}

void TxtReaderActivity::renderStatusBar() const {
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title);
}

void TxtReaderActivity::saveProgress(const bool isFinished) const {
  uint8_t data[5];
  data[0] = currentPage & 0xFF;
  data[1] = (currentPage >> 8) & 0xFF;
  data[2] = (currentPage >> 16) & 0xFF;
  data[3] = (currentPage >> 24) & 0xFF;
  data[4] = isFinished ? 1 : 0;
  ProgressFile::writeAtomic(txt->getCachePath(), data, sizeof(data));
}

void TxtReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[5] = {0};
    const int bytesRead = f.read(data, sizeof(data));
    if (bytesRead >= 4) {
      currentPage = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                    (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
      const bool isFinished = bytesRead >= 5 && data[4] == 1;
      LOG_DBG("TRS", "Loaded progress: page %d/%d, finished=%d", currentPage, totalPages, isFinished ? 1 : 0);
    }
  }
}

bool TxtReaderActivity::loadPageIndexCache() {
  // Cache file format (using serialization module):
  // - uint32_t: magic "TXTI"
  // - uint8_t: cache version
  // - uint32_t: file size (to validate cache)
  // - int32_t: viewport width
  // - int32_t: lines per page
  // - int32_t: font ID (to invalidate cache on font change)
  // - int32_t: screen margin (to invalidate cache on margin change)
  // - uint8_t: paragraph alignment (to invalidate cache on alignment change)
  // - uint32_t: total pages count
  // - N * uint32_t: page offsets

  std::string cachePath = txt->getCachePath() + "/index.bin";
  FsFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  // Read and validate header using serialization module
  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    return false;
  }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    LOG_DBG("TRS", "Cache screen margin mismatch, rebuilding");
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Loaded page index cache: %d pages", totalPages);
  return true;
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  FsFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  // Write header using serialization module
  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  LOG_DBG("TRS", "Saved page index cache: %d pages", totalPages);
}
