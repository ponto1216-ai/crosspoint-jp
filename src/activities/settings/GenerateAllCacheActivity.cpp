#include "GenerateAllCacheActivity.h"

#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <Epub/converters/ImageCacheValidation.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"
#include "util/CacheGenerationControls.h"

namespace {

// E-paper progress redraws are expensive (~670 ms in the measured run).
// Quarter-step updates keep useful feedback without dominating cache creation.
constexpr int CACHE_PROGRESS_STEP_PERCENT = 25;
constexpr int STATUS_BAR_CONTENT_GUARD = 8;

int getStatusBarContentReservation(const int statusBarHeight) {
  return statusBarHeight > 0 ? statusBarHeight + STATUS_BAR_CONTENT_GUARD : 0;
}

// Recursively scan a directory for EPUB files
bool findEpubFiles(const char* dirPath, std::vector<std::string>& results, CacheGenerationControls& controls,
                   GfxRenderer& renderer) {
  if (controls.shouldCancel(renderer)) return false;
  auto dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return true;
  }

  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (controls.shouldCancel(renderer)) {
      file.close();
      dir.close();
      return false;
    }
    file.getName(name, sizeof(name));
    if (name[0] == '.') {
      file.close();
      continue;
    }

    std::string fullPath = std::string(dirPath);
    if (fullPath.back() != '/') fullPath += '/';
    fullPath += name;

    if (file.isDirectory()) {
      file.close();
      if (!findEpubFiles(fullPath.c_str(), results, controls, renderer)) {
        dir.close();
        return false;
      }
    } else {
      if (FsHelpers::hasEpubExtension(std::string_view(name))) {
        results.push_back(fullPath);
      }
      file.close();
    }
  }
  dir.close();
  return true;
}

int pregeneratePixelCaches(const Page& page, GfxRenderer& renderer, const int xOffset, const int yOffset) {
  int generated = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageImage) continue;
    const auto& pageImage = static_cast<const PageImage&>(*element);
    const auto& image = pageImage.getImageBlock();
    if (image.pregeneratePixelCache(renderer, pageImage.xPos + xOffset, pageImage.yPos + yOffset)) generated++;
  }
  return generated;
}

int pregeneratePixelCachesFromCachedSection(Section& section, GfxRenderer& renderer, const int xOffset,
                                            const int yOffset, int& pagesScanned) {
  int generated = 0;
  for (uint16_t pageIndex = 0; pageIndex < section.pageCount; ++pageIndex) {
    auto page = section.loadPageFromSectionFile(pageIndex);
    if (!page) continue;
    ++pagesScanned;
    if (page->hasImages()) generated += pregeneratePixelCaches(*page, renderer, xOffset, yOffset);
  }
  return generated;
}

struct PixelCachePreflightResult {
  explicit PixelCachePreflightResult(const int spineCount) : sectionsNeedingPageScan(spineCount, false) {}

  std::vector<bool> sectionsNeedingPageScan;
  int sourceCount = 0;
  int validCacheCount = 0;
  int missingOrInvalidCacheCount = 0;
  int removedZeroLengthSourceCount = 0;
  bool complete = false;
};

bool isRasterImage(const std::string_view fileName) {
  return FsHelpers::hasPngExtension(fileName) || FsHelpers::hasJpgExtension(fileName);
}

bool parseRasterSourceSection(const std::string_view fileName, const int spineCount, int& sectionIndex) {
  constexpr std::string_view prefix = "img_";
  const size_t extensionStart = fileName.rfind('.');
  if (!isRasterImage(fileName) || extensionStart == std::string_view::npos || extensionStart <= prefix.size() ||
      fileName.substr(0, prefix.size()) != prefix) {
    return false;
  }

  size_t cursor = prefix.size();
  int parsedSectionIndex = 0;
  const size_t sectionStart = cursor;
  while (cursor < extensionStart && fileName[cursor] >= '0' && fileName[cursor] <= '9') {
    const int digit = fileName[cursor] - '0';
    if (parsedSectionIndex > (spineCount - 1) / 10) return false;
    parsedSectionIndex = parsedSectionIndex * 10 + digit;
    if (parsedSectionIndex >= spineCount) return false;
    ++cursor;
  }
  if (cursor == sectionStart || cursor >= extensionStart || fileName[cursor] != '_') return false;

  ++cursor;
  const size_t imageIndexStart = cursor;
  while (cursor < extensionStart && fileName[cursor] >= '0' && fileName[cursor] <= '9') ++cursor;
  if (cursor == imageIndexStart || cursor != extensionStart) return false;

  sectionIndex = parsedSectionIndex;
  return true;
}

PixelCachePreflightResult inspectPixelCaches(const std::string& cacheRoot, const int spineCount) {
  PixelCachePreflightResult result(spineCount);
  std::vector<std::string> zeroLengthSourcePaths;
  auto dir = Storage.open(cacheRoot.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return result;
  }

  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }

    if (file.getName(name, sizeof(name)) == 0) {
      file.close();
      dir.close();
      return result;
    }
    const size_t sourceSize = file.size();
    file.close();

    const std::string_view fileName(name);
    if (!isRasterImage(fileName)) continue;
    if (fileName.size() < 4 || fileName.substr(0, 4) != "img_") continue;

    int sectionIndex = 0;
    if (!parseRasterSourceSection(fileName, spineCount, sectionIndex)) {
      // Unexpected raster-image names make it unsafe to assume the directory scan was complete.
      dir.close();
      return result;
    }

    const std::string sourcePath = cacheRoot + "/" + name;
    if (sourceSize == 0) {
      zeroLengthSourcePaths.push_back(sourcePath);
      continue;
    }

    result.sourceCount++;
    const size_t extensionStart = sourcePath.rfind('.');
    const std::string pixelCachePath = sourcePath.substr(0, extensionStart) + ".pxc6";
    if (Storage.exists(pixelCachePath.c_str()) && ImageCacheValidation::validatePixelCacheFile(pixelCachePath, 0, 0)) {
      result.validCacheCount++;
    } else {
      result.missingOrInvalidCacheCount++;
      result.sectionsNeedingPageScan[sectionIndex] = true;
    }
  }

  dir.close();
  for (const auto& sourcePath : zeroLengthSourcePaths) {
    if (!Storage.remove(sourcePath.c_str())) {
      LOG_ERR("GENALL", "Failed to remove zero-length extracted image: %s", sourcePath.c_str());
      return result;
    }
    result.removedZeroLengthSourceCount++;
    LOG_DBG("GENALL", "Removed zero-length extracted image: %s", sourcePath.c_str());
  }
  result.complete = true;
  return result;
}

}  // namespace

void GenerateAllCacheActivity::onEnter() {
  Activity::onEnter();
  state = CONFIRMING;
  requestUpdate();
}

void GenerateAllCacheActivity::onExit() {
  Activity::onExit();
  // Release the SD card font caches built during cache generation.  The loop
  // clears caches *before* each book (max heap for layout), but never after
  // the last book, so its advance tables + prewarm data (~130KB) stay resident
  // and starve the large contiguous page buffer XTC needs (~96KB/page) when
  // another book is opened afterwards -> "memory error".  Free them here.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
    fcm->freeKernLigatureData();
  }
}

void GenerateAllCacheActivity::summarizeCacheStatuses(const std::vector<std::string>& epubFiles) {
  completeCount = 0;
  resumableCount = 0;
  notGeneratedCount = 0;
  for (const auto& epubPath : epubFiles) {
    switch (Epub(epubPath, "/.crosspoint").getCacheGenerationStatus()) {
      case Epub::CacheGenerationStatus::Complete:
        ++completeCount;
        break;
      case Epub::CacheGenerationStatus::Resumable:
        ++resumableCount;
        break;
      case Epub::CacheGenerationStatus::NotGenerated:
        ++notGeneratedCount;
        break;
    }
  }
}

std::string GenerateAllCacheActivity::cacheGenerationResultText() const {
  return std::string(tr(STR_CACHE_SUMMARY_COMPLETE)) + ": " + std::to_string(completeCount) + "  " +
         tr(STR_CACHE_SUMMARY_RESUMABLE) + ": " + std::to_string(resumableCount) + "  " +
         tr(STR_CACHE_SUMMARY_NOT_GENERATED) + ": " + std::to_string(notGeneratedCount);
}

void GenerateAllCacheActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageHeight = renderer.getScreenHeight();
  const auto layout = UiLayout::from(renderer);
  const int centerOffset = layout.content.x + layout.content.width / 2 - renderer.getScreenWidth() / 2;
  const auto drawCentered = [this, centerOffset](const int y, const char* text, const bool clear = true,
                                                 const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, y, text, clear, centerOffset, style);
  };

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{layout.content.x, metrics.topPadding, layout.content.width, metrics.headerHeight},
                 tr(STR_GENERATE_ALL_CACHE));

  if (state == CONFIRMING) {
    drawCentered(pageHeight / 2 - 20, tr(STR_GENERATE_CACHE));
    drawCentered(pageHeight / 2 + 10, tr(STR_GENERATE_CACHE_NOTE));

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == GENERATING) {
    drawCentered(pageHeight / 2, tr(STR_GENERATING_ALL_CACHE));
    drawCentered(pageHeight / 2 + 25, tr(STR_CACHE_CANCEL_HINT_LINE1));
    drawCentered(pageHeight / 2 + 45, tr(STR_CACHE_CANCEL_HINT_LINE2));
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    drawCentered(pageHeight / 2 - 20, tr(STR_CACHE_GENERATED), true, EpdFontFamily::BOLD);
    std::string resultText = cacheGenerationResultText();
    drawCentered(pageHeight / 2 + 10, resultText.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == INTERRUPTED) {
    drawCentered(pageHeight / 2 - 20, tr(STR_CACHE_INTERRUPTED), true, EpdFontFamily::BOLD);
    std::string resultText = cacheGenerationResultText();
    drawCentered(pageHeight / 2 + 10, resultText.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    drawCentered(pageHeight / 2 - 20, tr(STR_SD_CARD_ERROR), true, EpdFontFamily::BOLD);
    drawCentered(pageHeight / 2 + 10, tr(STR_CACHE_INTERRUPTED));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void GenerateAllCacheActivity::generateAllCaches() {
  const uint32_t generationStartedAt = millis();
  LOG_DBG("GENALL", "Scanning for EPUB files...");

  const uint32_t scanStartedAt = millis();
  std::vector<std::string> epubFiles;
  CacheGenerationControls controls;
  const bool scanCompleted = findEpubFiles("/", epubFiles, controls, renderer);
  LOG_DBG("GENALL", "EPUB scan completed in %lu ms", millis() - scanStartedAt);

  if (!scanCompleted) {
    LOG_DBG("GENALL", "Cancelled while scanning for EPUB files");
    totalCount = epubFiles.size();
    summarizeCacheStatuses(epubFiles);
    state = INTERRUPTED;
    requestUpdate();
    return;
  }

  totalCount = epubFiles.size();

  LOG_DBG("GENALL", "Found %d EPUB files", totalCount);

  if (totalCount == 0) {
    state = SUCCESS;
    requestUpdate();
    return;
  }

#if defined(CACHE_STORAGE_FAULT_INJECTION)
  struct CacheStorageFaultInjectionScope {
    CacheStorageFaultInjectionScope() {
      Section::setCacheStorageFaultInjectionActive(true);
      LOG_INF("SDFI", "event=armed point=temp_html_open spine=%d", CACHE_STORAGE_FAULT_SPINE);
    }
    ~CacheStorageFaultInjectionScope() { Section::setCacheStorageFaultInjectionActive(false); }
  } cacheStorageFaultInjectionScope;
#endif

  // Show progress popup
  const uint32_t initialDisplayStartedAt = millis();
  std::string progressDetail = std::string(tr(STR_CACHE_BOOK)) + " 0/" + std::to_string(totalCount);
  Rect popupRect = GUI.drawProgressPopup(renderer, tr(STR_GENERATING_ALL_CACHE), progressDetail.c_str());
  uint32_t progressDisplayMs = millis() - initialDisplayStartedAt;
  int lastDisplayedProgress = 0;
  bool cancelled = false;
  bool storageFailure = false;

  // Calculate viewport dimensions (screenMargin depends on writing direction, resolved per-book below)
  // Use a placeholder margin here; it will be recalculated per book after resolving isVertical.
  int orientedMarginTop = 0, orientedMarginRight = 0, orientedMarginBottom = 0, orientedMarginLeft = 0;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const int baseMarginTop = orientedMarginTop;
  const int baseMarginRight = orientedMarginRight;
  const int baseMarginBottom = orientedMarginBottom;
  const int baseMarginLeft = orientedMarginLeft;
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  for (int bookIdx = 0; bookIdx < totalCount; bookIdx++) {
    const auto& epubPath = epubFiles[bookIdx];
    const uint32_t bookStartedAt = millis();
    uint32_t sectionBuildMs = 0;
    uint32_t pixelCacheMs = 0;
    int sectionCacheHits = 0;
    int generatedSections = 0;
    int generatedPixelCaches = 0;
    int cachedPixelPagesScanned = 0;
    LOG_DBG("GENALL", "Processing %d/%d: %s", bookIdx + 1, totalCount, epubPath.c_str());

    const int progress = (bookIdx * 100) / totalCount;
    if (progress >= lastDisplayedProgress + CACHE_PROGRESS_STEP_PERCENT) {
      progressDetail =
          std::string(tr(STR_CACHE_BOOK)) + " " + std::to_string(bookIdx + 1) + "/" + std::to_string(totalCount);
      const uint32_t displayStartedAt = millis();
      GUI.updateProgressPopup(renderer, popupRect, progressDetail.c_str(), progress);
      progressDisplayMs += millis() - displayStartedAt;
      lastDisplayedProgress = progress;
    }

    if (controls.shouldCancel(renderer)) {
      LOG_DBG("GENALL", "Cancelled by user at book %d/%d", bookIdx + 1, totalCount);
      cancelled = true;
      break;
    }

    // Load EPUB
    auto epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
    if (!epub->load(true, SETTINGS.embeddedStyle == CrossPointSettings::CROSSPOINT_STYLE)) {
      LOG_ERR("GENALL", "Failed to load: %s", epubPath.c_str());
      continue;
    }

    const int spineCount = epub->getSpineItemsCount();
    if (spineCount <= 0) continue;
    epub->clearFullCacheGeneratedMarker();

    const uint32_t pixelPreflightStartedAt = millis();
    const auto pixelPreflight = inspectPixelCaches(epub->getCachePath(), spineCount);
    const uint32_t pixelPreflightMs = millis() - pixelPreflightStartedAt;
    pixelCacheMs += pixelPreflightMs;
    if (pixelPreflight.complete) {
      LOG_DBG("GENALL",
              "Raster cache preflight: sources=%d, valid=%d, missing/invalid=%d, removed-zero-length=%d, time=%lu ms",
              pixelPreflight.sourceCount, pixelPreflight.validCacheCount, pixelPreflight.missingOrInvalidCacheCount,
              pixelPreflight.removedZeroLengthSourceCount, pixelPreflightMs);
    } else {
      LOG_DBG("GENALL", "Raster cache preflight incomplete; using cached-page fallback (%lu ms)", pixelPreflightMs);
    }

    // Generate cover thumbnail
    const int coverHeight = UITheme::getInstance().getMetrics().homeCoverHeight;
    epub->generateThumbBmp(coverHeight);

    // Resolve writing mode
    bool isVertical = false;
    if (SETTINGS.writingMode == CrossPointSettings::WM_VERTICAL) {
      isVertical = true;
    } else if (SETTINGS.writingMode == CrossPointSettings::WM_HORIZONTAL) {
      isVertical = false;
    } else {
      isVertical = epub->isPageProgressionRtl() && (epub->getLanguage() == "ja" || epub->getLanguage() == "jpn" ||
                                                    epub->getLanguage() == "zh" || epub->getLanguage() == "zho");
    }

    const float lineCompression = SETTINGS.getReaderLineCompression(isVertical);
    renderer.setVerticalCharSpacing(SETTINGS.getVerticalCharSpacingPercent());

    auto* fcm = renderer.getFontCacheManager();
    if (fcm) {
      fcm->clearCache();
      fcm->freeKernLigatureData();
    }

    const auto& ds = SETTINGS.getDirectionSettings(isVertical);
    ensureSdFontLoaded(isVertical);
    configureRubyFont(isVertical);
    configureSmallFont(isVertical);

    // Calculate viewport dimensions with direction-specific margins
    const int bmTop = baseMarginTop + ds.screenMargin;
    const int bmRight = baseMarginRight + ds.screenMargin;
    const int bmLeft = baseMarginLeft + ds.screenMargin;
    const int bmBottom =
        baseMarginBottom + std::max(static_cast<int>(ds.screenMargin), getStatusBarContentReservation(statusBarHeight));
    const uint16_t viewportWidth = screenWidth - bmLeft - bmRight;
    const uint16_t viewportHeight = screenHeight - bmTop - bmBottom;

    const int headingFontIds[6] = {
        SETTINGS.getHeadingFontId(1, isVertical), SETTINGS.getHeadingFontId(2, isVertical), 0, 0, 0, 0};
    bool allSectionsReady = true;

    for (int i = 0; i < spineCount; i++) {
      if (controls.shouldCancel(renderer)) {
        LOG_DBG("GENALL", "Cancelled at section %d/%d of book %d/%d", i, spineCount, bookIdx + 1, totalCount);
        cancelled = true;
        allSectionsReady = false;
        break;
      }
      const int bookProgress = (i * 80) / spineCount;
      const int overallProgress = (bookIdx * 100 + bookProgress) / totalCount;
      if (overallProgress >= lastDisplayedProgress + CACHE_PROGRESS_STEP_PERCENT) {
        progressDetail = std::string(tr(STR_CACHE_BOOK)) + " " + std::to_string(bookIdx + 1) + "/" +
                         std::to_string(totalCount) + "  " + tr(STR_CACHE_CHAPTER) + " " + std::to_string(i + 1) + "/" +
                         std::to_string(spineCount);
        const uint32_t displayStartedAt = millis();
        GUI.updateProgressPopup(renderer, popupRect, progressDetail.c_str(), overallProgress);
        progressDisplayMs += millis() - displayStartedAt;
        lastDisplayedProgress = overallProgress;
      }
      Section sec(epub, i, renderer);
      const bool sectionCached =
          sec.loadSectionFile(SETTINGS.getReaderFontId(isVertical), SETTINGS.getTableFontId(isVertical),
                              lineCompression, ds.extraParagraphSpacing, ds.paragraphAlignment, viewportWidth,
                              viewportHeight, ds.hyphenationEnabled, ds.firstLineIndent, SETTINGS.embeddedStyle,
                              SETTINGS.imageRendering, isVertical, ds.charSpacing, ds.tateChuYokoMaxDigits);
      if (sectionCached) {
        sectionCacheHits++;
        // Read cached pages only when the directory preflight found a missing or
        // invalid raster-image cache. If preflight failed, preserve the fallback probe.
        bool needsPixelPageScan = pixelPreflight.complete && pixelPreflight.sectionsNeedingPageScan[i];
        if (!pixelPreflight.complete) {
          const std::string imagePrefix = epub->getCachePath() + "/img_" + std::to_string(i) + "_0";
          needsPixelPageScan = Storage.exists((imagePrefix + ".png").c_str()) ||
                               Storage.exists((imagePrefix + ".jpg").c_str()) ||
                               Storage.exists((imagePrefix + ".jpeg").c_str());
        }
        if (needsPixelPageScan) {
          const uint32_t pixelStartedAt = millis();
          generatedPixelCaches +=
              pregeneratePixelCachesFromCachedSection(sec, renderer, bmLeft, bmTop, cachedPixelPagesScanned);
          pixelCacheMs += millis() - pixelStartedAt;
        }
      } else {
        const uint32_t sectionStartedAt = millis();
        const int cssBodyFontIds[4] = {SETTINGS.getReaderFontIdForSize(isVertical, CrossPointSettings::SMALL),
                                       SETTINGS.getReaderFontIdForSize(isVertical, CrossPointSettings::MEDIUM),
                                       SETTINGS.getReaderFontIdForSize(isVertical, CrossPointSettings::LARGE),
                                       SETTINGS.getReaderFontIdForSize(isVertical, CrossPointSettings::EXTRA_LARGE)};
        if (!sec.createSectionFile(
                SETTINGS.getReaderFontId(isVertical), lineCompression, ds.extraParagraphSpacing, ds.paragraphAlignment,
                viewportWidth, viewportHeight, ds.hyphenationEnabled, ds.firstLineIndent, SETTINGS.embeddedStyle,
                SETTINGS.imageRendering, isVertical, ds.charSpacing, ds.tateChuYokoMaxDigits, nullptr, headingFontIds,
                SETTINGS.getTableFontId(isVertical), cssBodyFontIds, nullptr,
                [this, &generatedPixelCaches, &pixelCacheMs, bmLeft, bmTop](const Page& page) {
                  const uint32_t pixelStartedAt = millis();
                  generatedPixelCaches += pregeneratePixelCaches(page, renderer, bmLeft, bmTop);
                  pixelCacheMs += millis() - pixelStartedAt;
                },
                [&controls, this] { return controls.shouldCancel(renderer); })) {
          LOG_ERR("GENALL", "Failed section %d of %s", i, epubPath.c_str());
          allSectionsReady = false;
          const auto failureReason = sec.getLastCreateFailureReason();
          if (failureReason == Section::CreateFailureReason::Cancelled || controls.shouldCancel(renderer)) {
            cancelled = true;
            break;
          }
          if (failureReason == Section::CreateFailureReason::StorageIo) {
            LOG_ERR("GENALL", "Stopping cache generation after SD I/O failure at section %d of %s", i,
                    epubPath.c_str());
            storageFailure = true;
            break;
          }
          continue;
        }
        sectionBuildMs += millis() - sectionStartedAt;
        generatedSections++;
      }
    }

    if (cancelled || storageFailure) break;

    if (allSectionsReady) {
      if (!epub->markFullCacheGenerated()) {
        LOG_ERR("GENALL", "Could not publish completion marker: %s", epubPath.c_str());
        storageFailure = true;
      }
    } else {
      LOG_DBG("GENALL", "Cache incomplete for %s; a later run will resume it", epubPath.c_str());
    }

    LOG_DBG("GENALL",
            "Book timing: total=%lu ms, section-build=%lu ms (%d generated, %d cached), PXC=%lu ms (%d images, %d "
            "cached pages scanned)",
            millis() - bookStartedAt, sectionBuildMs, generatedSections, sectionCacheHits, pixelCacheMs,
            generatedPixelCaches, cachedPixelPagesScanned);
    if (cancelled || storageFailure) break;
  }

  if (!cancelled && !storageFailure) {
    const uint32_t finalDisplayStartedAt = millis();
    progressDetail = std::string(tr(STR_CACHE_COMPLETE));
    GUI.updateProgressPopup(renderer, popupRect, progressDetail.c_str(), 100);
    progressDisplayMs += millis() - finalDisplayStartedAt;
  }

  if (!storageFailure) {
    summarizeCacheStatuses(epubFiles);
  }

  LOG_DBG("GENALL", "Cache generation completed in %lu ms (progress display: %lu ms)", millis() - generationStartedAt,
          progressDisplayMs);
#if defined(CACHE_STORAGE_FAULT_INJECTION)
  if (storageFailure) {
    LOG_INF("SDFI", "event=safe_stop reason=storage_io");
  }
#endif
  state = storageFailure ? FAILED : (cancelled ? INTERRUPTED : SUCCESS);
  requestUpdate();
}

void GenerateAllCacheActivity::loop() {
  if (state == CONFIRMING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state = GENERATING;
      }
      requestUpdateAndWait();
      generateAllCaches();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == SUCCESS || state == INTERRUPTED || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }
}
