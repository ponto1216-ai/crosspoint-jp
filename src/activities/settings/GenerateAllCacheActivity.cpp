#include "GenerateAllCacheActivity.h"

#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <Epub/converters/ImageCacheValidation.h>
#include <Epub/converters/JpegCacheGenerator.h>
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

int pregeneratePngCaches(const Page& page, GfxRenderer& renderer) {
  int generated = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageImage) continue;
    const auto& image = static_cast<const PageImage&>(*element).getImageBlock();
    if (image.pregeneratePngCache(renderer)) generated++;
  }
  return generated;
}

int pregeneratePngCachesFromCachedSection(Section& section, GfxRenderer& renderer, int& pagesScanned) {
  int generated = 0;
  for (uint16_t pageIndex = 0; pageIndex < section.pageCount; ++pageIndex) {
    auto page = section.loadPageFromSectionFile(pageIndex);
    if (!page) continue;
    ++pagesScanned;
    if (page->hasImages()) generated += pregeneratePngCaches(*page, renderer);
  }
  return generated;
}

struct PngCachePreflightResult {
  explicit PngCachePreflightResult(const int spineCount) : sectionsNeedingPageScan(spineCount, false) {}

  std::vector<bool> sectionsNeedingPageScan;
  int sourceCount = 0;
  int validCacheCount = 0;
  int missingOrInvalidCacheCount = 0;
  int removedZeroLengthSourceCount = 0;
  bool complete = false;
};

bool parsePngSourceSection(const std::string_view fileName, const int spineCount, int& sectionIndex) {
  constexpr std::string_view prefix = "img_";
  constexpr size_t extensionLength = 4;
  if (!FsHelpers::hasPngExtension(fileName) || fileName.size() <= prefix.size() + extensionLength ||
      fileName.substr(0, prefix.size()) != prefix) {
    return false;
  }

  const size_t extensionStart = fileName.size() - extensionLength;
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

PngCachePreflightResult inspectPngCaches(const std::string& cacheRoot, const int spineCount) {
  PngCachePreflightResult result(spineCount);
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
    if (!FsHelpers::hasPngExtension(fileName)) continue;
    if (fileName.size() < 4 || fileName.substr(0, 4) != "img_") continue;

    int sectionIndex = 0;
    if (!parsePngSourceSection(fileName, spineCount, sectionIndex)) {
      // Unexpected PNG names make it unsafe to assume the directory scan was complete.
      dir.close();
      return result;
    }

    const std::string sourcePath = cacheRoot + "/" + name;
    if (sourceSize == 0) {
      zeroLengthSourcePaths.push_back(sourcePath);
      continue;
    }

    result.sourceCount++;
    const std::string pixelCachePath = sourcePath.substr(0, sourcePath.size() - 4) + ".pxc5";
    const std::string bmpCachePath = pixelCachePath + ".bmp";
    // PNG images now prefer a streamed BMP cache. Keep accepting a valid
    // legacy pixel cache so existing books do not regenerate unnecessarily.
    if ((Storage.exists(bmpCachePath.c_str()) && ImageCacheValidation::validateBmpCacheFile(bmpCachePath)) ||
        (Storage.exists(pixelCachePath.c_str()) && ImageCacheValidation::validatePixelCacheFile(pixelCachePath, 0, 0))) {
      result.validCacheCount++;
    } else {
      result.missingOrInvalidCacheCount++;
      result.sectionsNeedingPageScan[sectionIndex] = true;
    }
  }

  dir.close();
  for (const auto& sourcePath : zeroLengthSourcePaths) {
    if (!Storage.remove(sourcePath.c_str())) {
      LOG_ERR("GENALL", "Failed to remove zero-length extracted PNG: %s", sourcePath.c_str());
      return result;
    }
    result.removedZeroLengthSourceCount++;
    LOG_DBG("GENALL", "Removed zero-length extracted PNG: %s", sourcePath.c_str());
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
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GENERATE_ALL_CACHE));

  if (state == CONFIRMING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_GENERATE_CACHE), true);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, tr(STR_GENERATE_CACHE_NOTE), true);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == GENERATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_GENERATING_ALL_CACHE));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 25, tr(STR_CACHE_CANCEL_HINT_LINE1));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 45, tr(STR_CACHE_CANCEL_HINT_LINE2));
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CACHE_GENERATED), true, EpdFontFamily::BOLD);
    std::string resultText = cacheGenerationResultText();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, resultText.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == INTERRUPTED) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_CACHE_INTERRUPTED), true, EpdFontFamily::BOLD);
    std::string resultText = cacheGenerationResultText();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, resultText.c_str());
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

  // Show progress popup
  const uint32_t initialDisplayStartedAt = millis();
  std::string progressDetail = std::string(tr(STR_CACHE_BOOK)) + " 0/" + std::to_string(totalCount);
  Rect popupRect = GUI.drawProgressPopup(renderer, tr(STR_GENERATING_ALL_CACHE), progressDetail.c_str());
  uint32_t progressDisplayMs = millis() - initialDisplayStartedAt;
  int lastDisplayedProgress = 0;
  bool cancelled = false;

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
    uint32_t imageCacheMs = 0;
    uint32_t pngCacheMs = 0;
    int sectionCacheHits = 0;
    int generatedSections = 0;
    int generatedImageCaches = 0;
    int generatedPngCaches = 0;
    int cachedPngPagesScanned = 0;
    LOG_DBG("GENALL", "Processing %d/%d: %s", bookIdx + 1, totalCount, epubPath.c_str());

    const int progress = (bookIdx * 100) / totalCount;
    if (progress >= lastDisplayedProgress + CACHE_PROGRESS_STEP_PERCENT) {
      progressDetail = std::string(tr(STR_CACHE_BOOK)) + " " + std::to_string(bookIdx + 1) + "/" +
                       std::to_string(totalCount);
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

    const uint32_t pngPreflightStartedAt = millis();
    const auto pngPreflight = inspectPngCaches(epub->getCachePath(), spineCount);
    const uint32_t pngPreflightMs = millis() - pngPreflightStartedAt;
    pngCacheMs += pngPreflightMs;
    if (pngPreflight.complete) {
      LOG_DBG("GENALL",
              "PNG cache preflight: sources=%d, valid=%d, missing/invalid=%d, removed-zero-length=%d, time=%lu ms",
              pngPreflight.sourceCount, pngPreflight.validCacheCount, pngPreflight.missingOrInvalidCacheCount,
              pngPreflight.removedZeroLengthSourceCount, pngPreflightMs);
    } else {
      LOG_DBG("GENALL", "PNG cache preflight incomplete; using cached-page fallback (%lu ms)", pngPreflightMs);
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
    std::vector<bool> jpegEligibleSections(spineCount, false);
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
                         std::to_string(totalCount) + "  " + tr(STR_CACHE_CHAPTER) + " " +
                         std::to_string(i + 1) + "/" + std::to_string(spineCount);
        const uint32_t displayStartedAt = millis();
        GUI.updateProgressPopup(renderer, popupRect, progressDetail.c_str(), overallProgress);
        progressDisplayMs += millis() - displayStartedAt;
        lastDisplayedProgress = overallProgress;
      }
      Section sec(epub, i, renderer);
      const bool sectionCached = sec.loadSectionFile(
          SETTINGS.getReaderFontId(isVertical), lineCompression, ds.extraParagraphSpacing, ds.paragraphAlignment,
          viewportWidth, viewportHeight, ds.hyphenationEnabled, ds.firstLineIndent, SETTINGS.embeddedStyle,
          SETTINGS.imageRendering, isVertical, ds.charSpacing);
      if (sectionCached) {
        sectionCacheHits++;
        // JPEG caches are discovered directly from extracted image files below.
        // Read cached pages only when the directory preflight found a missing or
        // invalid PNG cache. If preflight failed, preserve the previous probe.
        bool needsPngPageScan = pngPreflight.complete && pngPreflight.sectionsNeedingPageScan[i];
        if (!pngPreflight.complete) {
          const std::string pngProbePath = epub->getCachePath() + "/img_" + std::to_string(i) + "_0.png";
          needsPngPageScan = Storage.exists(pngProbePath.c_str());
        }
        if (needsPngPageScan) {
          const uint32_t pngStartedAt = millis();
          generatedPngCaches += pregeneratePngCachesFromCachedSection(sec, renderer, cachedPngPagesScanned);
          pngCacheMs += millis() - pngStartedAt;
        }
      } else {
        const uint32_t sectionStartedAt = millis();
        const int cssBodyFontIds[4] = {SETTINGS.getReaderFontIdForSize(isVertical, CrossPointSettings::SMALL),
                                       SETTINGS.getReaderFontIdForSize(isVertical, CrossPointSettings::MEDIUM),
                                       SETTINGS.getReaderFontIdForSize(isVertical, CrossPointSettings::LARGE),
                                       SETTINGS.getReaderFontIdForSize(isVertical, CrossPointSettings::EXTRA_LARGE)};
        if (!sec.createSectionFile(SETTINGS.getReaderFontId(isVertical), lineCompression, ds.extraParagraphSpacing,
                                   ds.paragraphAlignment, viewportWidth, viewportHeight, ds.hyphenationEnabled,
                                   ds.firstLineIndent, SETTINGS.embeddedStyle, SETTINGS.imageRendering, isVertical,
                                   ds.charSpacing, nullptr, headingFontIds, SETTINGS.getTableFontId(isVertical),
                                   cssBodyFontIds, nullptr,
                                   [this, &generatedPngCaches, &pngCacheMs](const Page& page) {
                                     const uint32_t pngStartedAt = millis();
                                     generatedPngCaches += pregeneratePngCaches(page, renderer);
                                     pngCacheMs += millis() - pngStartedAt;
                                   },
                                   [&controls, this] { return controls.shouldCancel(renderer); })) {
          LOG_ERR("GENALL", "Failed section %d of %s", i, epubPath.c_str());
          allSectionsReady = false;
          continue;
        }
        sectionBuildMs += millis() - sectionStartedAt;
        generatedSections++;
      }
      jpegEligibleSections[i] = true;
    }

    if (cancelled) break;

    const uint32_t imageStartedAt = millis();
    const auto jpegResult = JpegCacheGenerator::generateFromExtractedImages(
        epub->getCachePath(), jpegEligibleSections, viewportWidth, viewportHeight, "GENALL", "GEN",
        [this, &cancelled, &controls, &progressDetail, &popupRect, &lastDisplayedProgress, &progressDisplayMs,
         bookIdx](const int done, const int total) {
          const int bookProgress = total > 0 ? 80 + (done * 20) / total : 100;
          const int overallProgress = (bookIdx * 100 + bookProgress) / this->totalCount;
          if (overallProgress >= lastDisplayedProgress + CACHE_PROGRESS_STEP_PERCENT || done == total) {
            progressDetail = std::string(tr(STR_CACHE_BOOK)) + " " + std::to_string(bookIdx + 1) + "/" +
                             std::to_string(this->totalCount) + "  " + tr(STR_CACHE_IMAGES) + " " +
                             std::to_string(done) + "/" + std::to_string(total);
            const uint32_t displayStartedAt = millis();
            GUI.updateProgressPopup(renderer, popupRect, progressDetail.c_str(), overallProgress);
            progressDisplayMs += millis() - displayStartedAt;
            lastDisplayedProgress = overallProgress;
          }
          cancelled = controls.shouldCancel(renderer);
          return !cancelled;
        });
    imageCacheMs += millis() - imageStartedAt;
    generatedImageCaches += jpegResult.generatedCacheCount;
    LOG_DBG("GENALL", "JPEG cache scan: sources=%d, valid=%d, generated=%d, invalid=%d, failed=%d, complete=%d",
            jpegResult.sourceCount, jpegResult.validCacheCount, jpegResult.generatedCacheCount,
            jpegResult.invalidCacheCount, jpegResult.failedCacheCount, jpegResult.scanComplete);
    if (allSectionsReady && jpegResult.scanComplete && jpegResult.failedCacheCount == 0) {
      if (!epub->markFullCacheGenerated()) LOG_ERR("GENALL", "Could not publish completion marker: %s", epubPath.c_str());
    } else {
      LOG_DBG("GENALL", "Cache incomplete for %s; a later run will resume it", epubPath.c_str());
    }

    LOG_DBG("GENALL",
            "Book timing: total=%lu ms, section-build=%lu ms (%d generated, %d cached), JPEG-BMP=%lu ms (%d images), PNG=%lu ms (%d images, %d cached pages scanned)",
            millis() - bookStartedAt, sectionBuildMs, generatedSections, sectionCacheHits, imageCacheMs,
            generatedImageCaches, pngCacheMs, generatedPngCaches, cachedPngPagesScanned);
    if (cancelled) break;
  }

  if (!cancelled) {
    const uint32_t finalDisplayStartedAt = millis();
    progressDetail = std::string(tr(STR_CACHE_COMPLETE));
    GUI.updateProgressPopup(renderer, popupRect, progressDetail.c_str(), 100);
    progressDisplayMs += millis() - finalDisplayStartedAt;
  }

  summarizeCacheStatuses(epubFiles);

  LOG_DBG("GENALL", "Cache generation completed in %lu ms (progress display: %lu ms)",
          millis() - generationStartedAt, progressDisplayMs);
  state = cancelled ? INTERRUPTED : SUCCESS;
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

  if (state == SUCCESS || state == INTERRUPTED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }
}
