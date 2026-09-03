#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/converters/JpegCacheGenerator.h>
#include <FontCacheManager.h>
#include <FontManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Issue18Diagnostics.h>
#include <Logging.h>
#include <esp_system.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "BookCacheClearActivity.h"
#include "BookReaderSettings.h"
#include "BookReaderSettingsActivity.h"
#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "activities/settings/DiagnosticsActivity.h"
#include "activities/settings/FontSelectionActivity.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderDetailsActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "JsonSettingsIO.h"
#include "MappedInputManager.h"
#include "OrientationHelper.h"
#include "ProgressFile.h"
#include "ReadingHistoryStore.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "ReadingStatusHelper.h"
#include "SdCardFontGlobals.h"
#include "activities/settings/LineSpacingSelectionActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/settings/StatusBarSettingsActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/BookDataPath.h"
#include "util/CacheGenerationControls.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long skipChapterMs = 700;
// E-paper progress redraws are expensive (~670 ms in the measured run).
// Quarter-step updates keep useful feedback without dominating cache creation.
constexpr int CACHE_PROGRESS_STEP_PERCENT = 25;
// Vertical glyph bounds can extend a few pixels past their layout advance.
// Keep this guard between reader content and a visible status bar.
constexpr int STATUS_BAR_CONTENT_GUARD = 8;
constexpr size_t MAX_BOOKMARKS_PER_BOOK = 24;
constexpr float BOOKMARK_PROGRESS_EPSILON = 0.0001f;
// Small, short EPUBs are quick to build lazily as the reader reaches each
// section. Avoid interrupting their first open with a full-cache prompt.
constexpr size_t SMALL_BOOK_CACHE_PROMPT_MAX_TEXT_BYTES = 256 * 1024;
constexpr int SMALL_BOOK_CACHE_PROMPT_MAX_SPINE_ITEMS = 10;
// pages per minute, first item is 1 to prevent division by zero if accessed
const std::vector<int> PAGE_TURN_LABELS = {1, 1, 3, 6, 12};

int getStatusBarContentReservation(const int statusBarHeight) {
  return statusBarHeight > 0 ? statusBarHeight + STATUS_BAR_CONTENT_GUARD : 0;
}

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

bool shouldSkipInitialCachePrompt(const Epub& epub) {
  const int spineCount = epub.getSpineItemsCount();
  const size_t textSize = epub.getBookSize();
  return spineCount > 0 && spineCount <= SMALL_BOOK_CACHE_PROMPT_MAX_SPINE_ITEMS && textSize > 0 &&
         textSize <= SMALL_BOOK_CACHE_PROMPT_MAX_TEXT_BYTES;
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getBookmarkPageRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  return {epub->calculateProgress(spineIndex, std::max(0.0f, anchor - step * 0.5f)),
          epub->calculateProgress(spineIndex, std::min(1.0f, anchor + step * 0.5f))};
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

}  // namespace

void EpubReaderActivity::pregenerateCache() {
  CacheGenerationControls controls;
  const uint32_t generationStartedAt = millis();
  uint32_t sectionBuildMs = 0;
  uint32_t imageCacheMs = 0;
  uint32_t pngCacheMs = 0;
  int sectionCacheHits = 0;
  int generatedSections = 0;
  int generatedImageCaches = 0;
  int generatedPngCaches = 0;
  if (!epub) return;

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0) return;
  // Drop the completion state before work begins.  A reset/crash then resumes
  // through validated per-section files instead of treating a partial run as done.
  epub->clearFullCacheGeneratedMarker();

  bool isVertical = false;
  if (SETTINGS.writingMode == CrossPointSettings::WM_VERTICAL) {
    isVertical = true;
  } else if (SETTINGS.writingMode == CrossPointSettings::WM_HORIZONTAL) {
    isVertical = false;
  } else {
    isVertical = epub->isPageProgressionRtl() && (epub->getLanguage() == "ja" || epub->getLanguage() == "jpn" ||
                                                  epub->getLanguage() == "zh" || epub->getLanguage() == "zho");
  }

  const auto& ds = SETTINGS.getDirectionSettings(isVertical);
  ensureSdFontLoaded(isVertical);
  configureRubyFont(isVertical);

  int orientedMarginTop = 0, orientedMarginRight = 0, orientedMarginBottom = 0, orientedMarginLeft = 0;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += ds.screenMargin;
  orientedMarginRight += ds.screenMargin;
  orientedMarginLeft += ds.screenMargin;
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  orientedMarginBottom += std::max(static_cast<int>(ds.screenMargin), getStatusBarContentReservation(statusBarHeight));

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  const float lineCompression = SETTINGS.getReaderLineCompression(isVertical);
  renderer.setVerticalCharSpacing(SETTINGS.getVerticalCharSpacingPercent());

  auto* fcm = renderer.getFontCacheManager();
  if (fcm) {
    fcm->clearCache();
    fcm->freeKernLigatureData();
  }

  const int headingFontIds[6] = {
      SETTINGS.getHeadingFontId(1, isVertical), SETTINGS.getHeadingFontId(2, isVertical), 0, 0, 0, 0};

  const uint32_t initialDisplayStartedAt = millis();
  std::string progressDetail = std::string(tr(STR_CACHE_CHAPTER)) + " 0/" + std::to_string(spineCount);
  renderer.clearScreen();
  const int screenCenterY = renderer.getScreenHeight() / 2;
  renderer.drawCenteredText(UI_10_FONT_ID, screenCenterY, tr(STR_GENERATING_CACHE));
  renderer.drawCenteredText(UI_10_FONT_ID, screenCenterY + 25, tr(STR_CACHE_CANCEL_HINT_LINE1));
  renderer.drawCenteredText(UI_10_FONT_ID, screenCenterY + 45, tr(STR_CACHE_CANCEL_HINT_LINE2));
  Rect popupRect = GUI.drawProgressPopup(renderer, tr(STR_GENERATING_CACHE), progressDetail.c_str());
  uint32_t progressDisplayMs = millis() - initialDisplayStartedAt;
  int lastDisplayedProgress = 0;
  bool cancelled = false;
  std::vector<bool> jpegEligibleSections(spineCount, false);

  for (int i = 0; i < spineCount; i++) {
    if (controls.shouldCancel(renderer)) {
      LOG_DBG("ERS", "Pregenerate cancelled at section %d/%d", i, spineCount);
      cancelled = true;
      break;
    }

    const int progress = (i * 80) / spineCount;
    if (progress >= lastDisplayedProgress + CACHE_PROGRESS_STEP_PERCENT) {
      progressDetail =
          std::string(tr(STR_CACHE_CHAPTER)) + " " + std::to_string(i + 1) + "/" + std::to_string(spineCount);
      const uint32_t displayStartedAt = millis();
      GUI.updateProgressPopup(renderer, popupRect, progressDetail.c_str(), progress);
      progressDisplayMs += millis() - displayStartedAt;
      lastDisplayedProgress = progress;
    }

    Section sec(epub, i, renderer);
    const bool sectionCached = sec.loadSectionFile(
        SETTINGS.getReaderFontId(isVertical), lineCompression, ds.extraParagraphSpacing, ds.paragraphAlignment,
        viewportWidth, viewportHeight, ds.hyphenationEnabled, ds.firstLineIndent, SETTINGS.embeddedStyle,
        SETTINGS.imageRendering, isVertical, ds.charSpacing);
    if (sectionCached) {
      sectionCacheHits++;
    } else {
      const uint32_t sectionStartedAt = millis();
      bool cancelledDuringSection = false;
      const int cssBodyFontIds[4] = {SETTINGS.getReaderFontIdForSize(isVertical, CrossPointSettings::SMALL),
                                     SETTINGS.getReaderFontIdForSize(isVertical, CrossPointSettings::MEDIUM),
                                     SETTINGS.getReaderFontIdForSize(isVertical, CrossPointSettings::LARGE),
                                     SETTINGS.getReaderFontIdForSize(isVertical, CrossPointSettings::EXTRA_LARGE)};
      if (!sec.createSectionFile(
              SETTINGS.getReaderFontId(isVertical), lineCompression, ds.extraParagraphSpacing, ds.paragraphAlignment,
              viewportWidth, viewportHeight, ds.hyphenationEnabled, ds.firstLineIndent, SETTINGS.embeddedStyle,
              SETTINGS.imageRendering, isVertical, ds.charSpacing, nullptr, headingFontIds,
              SETTINGS.getTableFontId(isVertical), cssBodyFontIds, nullptr,
              [this, &generatedPngCaches, &pngCacheMs](const Page& page) {
                const uint32_t pngStartedAt = millis();
                generatedPngCaches += pregeneratePngCaches(page, renderer);
                pngCacheMs += millis() - pngStartedAt;
              },
              [&cancelledDuringSection, &controls, this] {
                cancelledDuringSection = controls.shouldCancel(renderer);
                return cancelledDuringSection;
              })) {
        if (cancelledDuringSection) {
          LOG_DBG("ERS", "Pregenerate cancelled while building section %d/%d", i, spineCount);
          cancelled = true;
          break;
        }
        LOG_ERR("ERS", "Pregenerate: failed section %d (heap: %d)", i, ESP.getFreeHeap());
        continue;
      }
      sectionBuildMs += millis() - sectionStartedAt;
      generatedSections++;
    }
    jpegEligibleSections[i] = true;
  }

  bool imagesComplete = false;
  if (!cancelled) {
    const uint32_t imageStartedAt = millis();
    const auto jpegResult = JpegCacheGenerator::generateFromExtractedImages(
        epub->getCachePath(), jpegEligibleSections, viewportWidth, viewportHeight, "ERS", "PRE",
        [this, &cancelled, &controls, &progressDetail, &popupRect, &lastDisplayedProgress, &progressDisplayMs](
            const int done, const int total) {
          const int progress = total > 0 ? 80 + (done * 20) / total : 100;
          if (progress >= lastDisplayedProgress + CACHE_PROGRESS_STEP_PERCENT || done == total) {
            progressDetail =
                std::string(tr(STR_CACHE_IMAGES)) + " " + std::to_string(done) + "/" + std::to_string(total);
            const uint32_t displayStartedAt = millis();
            GUI.updateProgressPopup(renderer, popupRect, progressDetail.c_str(), progress);
            progressDisplayMs += millis() - displayStartedAt;
            lastDisplayedProgress = progress;
          }
          cancelled = controls.shouldCancel(renderer);
          return !cancelled;
        });
    imageCacheMs += millis() - imageStartedAt;
    generatedImageCaches += jpegResult.generatedCacheCount;
    LOG_DBG("ERS", "JPEG cache scan: sources=%d, valid=%d, generated=%d, invalid=%d, failed=%d, complete=%d",
            jpegResult.sourceCount, jpegResult.validCacheCount, jpegResult.generatedCacheCount,
            jpegResult.invalidCacheCount, jpegResult.failedCacheCount, jpegResult.scanComplete);
    imagesComplete = jpegResult.scanComplete && jpegResult.failedCacheCount == 0;
  }

  if (!cancelled && generatedSections + sectionCacheHits == spineCount && imagesComplete) {
    if (!epub->markFullCacheGenerated()) LOG_ERR("ERS", "Could not publish full-cache completion marker");
  } else {
    LOG_DBG("ERS", "Full cache remains incomplete; next run will resume missing work");
  }

  if (!cancelled) {
    const uint32_t finalDisplayStartedAt = millis();
    progressDetail = std::string(tr(STR_CACHE_IMAGES)) + " " + tr(STR_CACHE_COMPLETE);
    GUI.updateProgressPopup(renderer, popupRect, progressDetail.c_str(), 100);
    progressDisplayMs += millis() - finalDisplayStartedAt;
  }
  LOG_DBG("ERS",
          "Pregenerate timing: total=%lu ms, section-build=%lu ms (%d generated, %d cached), JPEG-BMP=%lu ms (%d "
          "images), PNG=%lu ms (%d images), progress=%lu ms",
          millis() - generationStartedAt, sectionBuildMs, generatedSections, sectionCacheHits, imageCacheMs,
          generatedImageCaches, pngCacheMs, generatedPngCaches, progressDisplayMs);
}

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  Issue18Diagnostics::logMemory("reader-enter", epub->getPath().c_str());

  // ルビフォントIDはrender()内でフォントロード後に設定

  // Screen orientation (both renderer and input) is already set by
  // enterNewActivity() → OrientationHelper::applyOrientation() before onEnter().

  epub->setupCacheDir();
  // Opening a book can change its progress or invalidate a complete cache
  // marker. Drop the browser's summary entry once; it will be refreshed from
  // the authoritative per-book files on the next folder view.
  invalidateBookListStatusIndexEntry(epub->getPath(), "/.crosspoint");
  loadCachedBookmarks();

  const std::string legacyProgressPath = epub->getCachePath() + "/progress.bin";
  uint64_t bookId = 0;
  const bool hasBookId = epub->getSourceFingerprint(&bookId);
  const std::string progressPath = hasBookId ? BookDataPath::getProgressPath(bookId) : legacyProgressPath;
  uint8_t data[7] = {};
  size_t dataSize = 0;
  if (Storage.exists(progressPath.c_str())) {
    dataSize = ProgressFile::readLegacyCompatible(progressPath, data);
  } else if (hasBookId) {
    dataSize = ProgressFile::readLegacyCompatible(legacyProgressPath, data);
    if (dataSize != 0 && BookDataPath::ensureDirectory(bookId) &&
        ProgressFile::writeAtomicPath(progressPath, data, dataSize)) {
      LOG_INF("ERS", "Migrated progress to BookId %016llx", static_cast<unsigned long long>(bookId));
    }
  } else {
    dataSize = ProgressFile::readLegacyCompatible(legacyProgressPath, data);
  }
  if (dataSize != 0) {
    if (dataSize == 4 || dataSize == 6 || dataSize == 7) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6 || dataSize == 7) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath(), bookId);
  const auto beginReadingSession = [this, bookId] {
    if (epub) READING_HISTORY.beginSession(epub->getPath(), epub->getTitle(), epub->getAuthor(), bookId);
  };

  // Showing the prompt once is enough.  A cancelled generation remains resumable
  // from the Reader menu, so reopening the book must not interrupt reading again.
  const std::string cachePromptSeenPath = epub->getCachePath() + "/.cache_prompt_seen";
  const std::string legacyNoCachePromptPath = epub->getCachePath() + "/.no_cache_prompt";
  if (!epub->isFullCacheGenerated() && !shouldSkipInitialCachePrompt(*epub) &&
      !Storage.exists(cachePromptSeenPath.c_str()) && !Storage.exists(legacyNoCachePromptPath.c_str())) {
    FsFile promptMarker;
    if (!Storage.openFileForWrite("ERS", cachePromptSeenPath, promptMarker)) {
      LOG_ERR("ERS", "Could not record initial cache prompt");
    } else {
      promptMarker.close();
    }

    auto handler = [this, beginReadingSession](const ActivityResult& res) {
      if (!res.isCancelled) {
        pregenerateCache();
        // Do not attribute the cache build to the book.  The session begins
        // only once the reader can show the actual text.
        beginReadingSession();
        requestUpdate();
      } else {
        // Left means "Later". Back keeps the existing close-book behavior.
        if (std::holds_alternative<MenuResult>(res.data)) {
          beginReadingSession();
          requestUpdate();
        } else {
          onGoHome();
        }
      }
    };
    startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_GENERATE_CACHE),
                                               tr(STR_GENERATE_CACHE_NOTE), tr(STR_SKIP_CACHE), tr(STR_GENERATE)),
        handler);
    return;
  }

  beginReadingSession();
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();
  READING_HISTORY.endSession();

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  epub.reset();

  // The reader's SD font data is rebuilt when another book is opened.  Release
  // it before Home allocates its cover buffer so fragmented page memory does
  // not make the menu fail to render its recent-book cover.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
    fcm->freeKernLigatureData();
  }

  // A book override is only an in-memory effective configuration.  Restore
  // the persisted Global reader settings before returning to the rest of UI.
  if (restoreGlobalReaderSettingsOnExit && !SETTINGS.loadFromFile()) {
    LOG_ERR("BOOKSET", "Could not restore Global reader settings after closing book");
  }
}

bool EpubReaderActivity::saveBookDirectionFields(const uint16_t fields) {
  if (activeBookFingerprint == 0) {
    return SETTINGS.saveToFile();
  }

  BookReaderSettings::Override value;
  if (!BookReaderSettings::load(activeBookFingerprint, value)) {
    LOG_ERR("BOOKSET", "Could not load reader override for save (%016llx)",
            static_cast<unsigned long long>(activeBookFingerprint));
    return false;
  }

  const auto current = BookReaderSettings::captureAll(SETTINGS);
  auto& target = verticalMode ? value.vertical : value.horizontal;
  const auto& source = verticalMode ? current.vertical : current.horizontal;
  target.values = source.values;
  target.fields |= fields;
  const bool saved = BookReaderSettings::save(activeBookFingerprint, value);
  if (saved) restoreGlobalReaderSettingsOnExit = true;
  return saved;
}

bool EpubReaderActivity::saveBookGlobalField(const uint16_t field) {
  if (activeBookFingerprint == 0) {
    return SETTINGS.saveToFile();
  }

  BookReaderSettings::Override value;
  if (!BookReaderSettings::load(activeBookFingerprint, value)) {
    LOG_ERR("BOOKSET", "Could not load reader override for global save (%016llx)",
            static_cast<unsigned long long>(activeBookFingerprint));
    return false;
  }

  if (field == BookReaderSettings::Orientation) value.orientation = SETTINGS.orientation;
  if (field == BookReaderSettings::WritingMode) value.writingMode = SETTINGS.writingMode;
  if (field == BookReaderSettings::BookStyle) value.bookStyle = SETTINGS.embeddedStyle;
  if (field == BookReaderSettings::ImageRendering) value.imageRendering = SETTINGS.imageRendering;
  if (field == BookReaderSettings::InvertImages) value.invertImages = SETTINGS.invertImages;
  value.fields |= field;
  const bool saved = BookReaderSettings::save(activeBookFingerprint, value);
  if (saved) restoreGlobalReaderSettingsOnExit = true;
  return saved;
}

void EpubReaderActivity::restoreActiveBookOverride() {
  if (!restoreGlobalReaderSettingsOnExit || activeBookFingerprint == 0) return;

  BookReaderSettings::Override value;
  if (BookReaderSettings::load(activeBookFingerprint, value) && BookReaderSettings::hasAnyField(value)) {
    BookReaderSettings::apply(value, SETTINGS);
  } else {
    LOG_ERR("BOOKSET", "Could not restore reader override for %016llx",
            static_cast<unsigned long long>(activeBookFingerprint));
  }
}

void EpubReaderActivity::loop() {
  READING_HISTORY.tick();
  if (mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased()) READING_HISTORY.noteInteraction();
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  if (rubyAdjustActive) {
    if (RenderLock::peek()) return;
    if (rubyAdjustIgnoreOpeningRelease) {
      if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
          !mappedInput.isPressed(MappedInputManager::Button::Back)) {
        rubyAdjustIgnoreOpeningRelease = false;
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      exitRubyAdjustMode();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      adjustRubyOffset(RubyAdjustAxis::X, -1);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      adjustRubyOffset(RubyAdjustAxis::X, 1);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      adjustRubyOffset(RubyAdjustAxis::Y, -1);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      adjustRubyOffset(RubyAdjustAxis::Y, 1);
      return;
    }
    return;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  // Enter reader menu activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Snapshot reader state under render lock. This avoids racing with the
    // render task while it may rebuild/reset the current section.
    int menuSpineIndex = 0;
    int menuCurrentPage = 0;
    int menuTotalPages = 0;
    {
      RenderLock lock(*this);
      menuSpineIndex = currentSpineIndex;
      if (section) {
        menuCurrentPage = section->currentPage + 1;
        menuTotalPages = section->pageCount;
      }
    }

    if (!epub) return;

    float bookProgress = 0.0f;
    if (epub->getBookSize() > 0 && menuTotalPages > 0) {
      const float chapterProgress = static_cast<float>(menuCurrentPage - 1) / static_cast<float>(menuTotalPages);
      bookProgress = epub->calculateProgress(menuSpineIndex, chapterProgress) * 100.0f;
    }
    const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
    startActivityForResult(
        std::make_unique<EpubReaderMenuActivity>(
            renderer, mappedInput, epub->getTitle(), menuCurrentPage, menuTotalPages, bookProgressPercent,
            SETTINGS.orientation, verticalMode, !cachedBookmarks.empty(), epub->getCacheGenerationStatus(),
            [this] { saveBookDirectionFields(BookReaderSettings::DirectionIndent); },
            [this] { saveBookGlobalField(BookReaderSettings::InvertImages); }),
        [this](const ActivityResult& result) {
          // Always apply orientation change even if the menu was cancelled
          const auto& menu = std::get<MenuResult>(result.data);
          applyOrientation(menu.orientation);
          toggleAutoPageTurn(menu.pageTurnOption);
          if (menu.layoutChanged) {
            invalidateSectionPreservingPosition();
            requestUpdate();
          }
          if (!result.isCancelled) {
            onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
          }
        });
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  const auto orientation = renderer.getOrientation();
  const bool reverseSideButtons = verticalMode && (orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                                                   orientation == GfxRenderer::Orientation::LandscapeCounterClockwise);
  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput, reverseSideButtons);
  (void)fromTilt;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // Keep the normal reading direction at the end screen: the forward button
  // closes the book, while the back button returns to the last page.
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    const bool closeBookTriggered = verticalMode ? prevTriggered : nextTriggered;
    if (closeBookTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = UINT16_MAX;
      requestUpdate();
    }
    return;
  }

  const bool skipChapter = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipChapterMs;

  if (skipChapter) {
    // If there is no adjacent chapter in the requested direction, leave the
    // normal page-turn path in charge.  Assigning an out-of-range spine index
    // here previously wrapped a one-chapter book to its beginning or end.
    const bool skipForward = verticalMode ? !nextTriggered : nextTriggered;
    const int targetSpineIndex = skipForward ? currentSpineIndex + 1 : currentSpineIndex - 1;
    if (targetSpineIndex >= 0 && targetSpineIndex < epub->getSpineItemsCount()) {
      lastPageTurnTime = millis();
      // We don't want to delete the section mid-render, so grab the semaphore.
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex = targetSpineIndex;
        section.reset();
      }
      requestUpdate();
      return;
    }
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(verticalMode);  // In vertical RTL: prev button = forward
  } else {
    pageTurn(!verticalMode);  // In vertical RTL: next button = backward
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  jumpToBookProgress(static_cast<float>(clampPercent(percent)) / 100.0f);
}

void EpubReaderActivity::jumpToBookProgress(float progress) {
  if (!epub || epub->getBookSize() == 0) return;
  progress = std::clamp(progress, 0.0f, 1.0f);
  const size_t bookSize = epub->getBookSize();
  // Convert normalized progress into an absolute position across the spine sizes.
  size_t targetSize = static_cast<size_t>(progress * static_cast<float>(bookSize));
  if (targetSize >= bookSize) targetSize = bookSize - 1;

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    clearDeferredReposition();
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
}

void EpubReaderActivity::invalidateSectionPreservingPosition() {
  RenderLock lock(*this);
  if (section) {
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
    section.reset();
  }
}

void EpubReaderActivity::clearDeferredReposition() {
  cachedChapterTotalPageCount = 0;
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          [this](const ActivityResult& result) {
            loadCachedBookmarks();
            if (result.isCancelled) return;
            const auto& bookmark = std::get<BookmarkResult>(result.data);
            if (bookmark.spineIndex == currentSpineIndex && section &&
                bookmark.chapterPageCount == section->pageCount) {
              RenderLock lock(*this);
              clearDeferredReposition();
              section->currentPage = std::min<int>(bookmark.chapterPage, section->pageCount - 1);
            } else {
              jumpToBookProgress(bookmark.percentage);
            }
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK:
      toggleBookmark();
      break;
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled && currentSpineIndex != std::get<ChapterResult>(result.data).spineIndex) {
              RenderLock lock(*this);
              clearDeferredReposition();
              currentSpineIndex = std::get<ChapterResult>(result.data).spineIndex;
              nextPageNumber = 0;
              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::READER_SETTINGS: {
      startActivityForResult(std::make_unique<EpubReaderDetailsActivity>(renderer, mappedInput),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& menu = std::get<MenuResult>(result.data);
                                 onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                               }
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::OPEN_GLOBAL_READER_SETTINGS: {
      // Open settings directly on the Reader category and select the submenu
      // that matches this book's current writing mode.
      // Index 0 is the Reader tab itself; the direction settings are its
      // first and second items.
      const int directionSettingIndex = verticalMode ? 2 : 1;
      // Edit the persisted Global profile while the book override is
      // suspended.  Otherwise SettingsActivity would overwrite the effective
      // book values and make it impossible to change Global independently.
      if (restoreGlobalReaderSettingsOnExit && !SETTINGS.loadFromFile()) {
        LOG_ERR("BOOKSET", "Could not suspend book override before Global settings");
        break;
      }
      startActivityForResult(std::make_unique<SettingsActivity>(
                                 renderer, mappedInput, [this] { finish(); }, 1, directionSettingIndex),
                             [this](const ActivityResult&) {
                               restoreActiveBookOverride();
                               // Reader settings (font/line spacing/margins etc.) may change pagination.
                               // Cache dir may have been deleted by ClearCacheActivity — recreate it.
                               if (epub) epub->setupCacheDir();
                               // A profile can select a different installed SD font. Reload the
                               // family for this book's active writing direction before resolving
                               // font IDs and rebuilding the current section.
                               ensureSdFontLoaded(verticalMode);
                               configureRubyFont(verticalMode);
                               invalidateSectionPreservingPosition();
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STYLE_FONT_FAMILY: {
      const auto& currentFont = SETTINGS.getDirectionSettings(verticalMode);
      const uint8_t originalFontFamily = currentFont.fontFamily;
      const std::string originalSdFontFamilyName = currentFont.sdFontFamilyName;
      startActivityForResult(
          std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry(), verticalMode),
          [this, originalFontFamily, originalSdFontFamilyName](const ActivityResult&) {
            const auto& updatedFont = SETTINGS.getDirectionSettings(verticalMode);
            if (updatedFont.fontFamily != originalFontFamily ||
                originalSdFontFamilyName != updatedFont.sdFontFamilyName) {
              saveBookDirectionFields(BookReaderSettings::DirectionFont);
            }
            ensureSdFontLoaded(verticalMode);
            configureRubyFont(verticalMode);
            invalidateSectionPreservingPosition();
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STYLE_FIRST_LINE_INDENT: {
      auto& dirSettings = SETTINGS.getDirectionSettings(verticalMode);
      dirSettings.firstLineIndent = !dirSettings.firstLineIndent;
      saveBookDirectionFields(BookReaderSettings::DirectionIndent);
      invalidateSectionPreservingPosition();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STYLE_INVERT_IMAGES: {
      SETTINGS.invertImages = !SETTINGS.invertImages;
      saveBookGlobalField(BookReaderSettings::InvertImages);
      renderer.setInvertImagesInDarkMode(SETTINGS.invertImages);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STYLE_LINE_SPACING: {
      uint8_t& target = SETTINGS.getDirectionSettings(verticalMode).lineSpacing;
      startActivityForResult(std::make_unique<LineSpacingSelectionActivity>(
                                 renderer, mappedInput, static_cast<int>(target),
                                 [this, &target](const int selectedValue) {
                                   target = static_cast<uint8_t>(selectedValue);
                                   saveBookDirectionFields(BookReaderSettings::DirectionLineSpacing);
                                   finish();
                                 },
                                 [this] { finish(); }),
                             [this](const ActivityResult&) {
                               invalidateSectionPreservingPosition();
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STYLE_STATUS_BAR: {
      startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::RUBY_OFFSET:
      enterRubyAdjustMode();
      break;
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        auto p = section->loadPageFromSectionFile();
        if (p) {
          std::string fullText;
          for (const auto& el : p->elements) {
            if (el->getTag() == TAG_PageLine) {
              const auto& line = static_cast<const PageLine&>(*el);
              if (line.getBlock()) {
                const auto& words = line.getBlock()->getWords();
                for (const auto& w : words) {
                  if (!fullText.empty()) fullText += " ";
                  fullText += w;
                }
              }
            }
          }
          if (!fullText.empty()) {
            startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                   [this](const ActivityResult& result) {});
            break;
          }
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::OPEN_BOOK_READER_SETTINGS: {
      uint64_t fingerprint = 0;
      if (!epub->getSourceFingerprint(&fingerprint)) break;
      startActivityForResult(std::make_unique<BookReaderSettingsActivity>(renderer, mappedInput, fingerprint, verticalMode),
                             [this, fingerprint](const ActivityResult& result) {
                               if (result.isCancelled) return;
                               BookReaderSettings::Override bookOverride;
                               const bool hasOverride = BookReaderSettings::load(fingerprint, bookOverride) &&
                                                        BookReaderSettings::hasAnyField(bookOverride);
                               if (hasOverride) {
                                 BookReaderSettings::apply(bookOverride, SETTINGS);
                               } else if (!SETTINGS.loadFromFile()) {
                                 LOG_ERR("BOOKSET", "Could not restore Global settings after clearing override");
                                 return;
                               }
                               restoreGlobalReaderSettingsOnExit = hasOverride;
                               ensureSdFontLoaded(verticalMode);
                               configureRubyFont(verticalMode);
                               invalidateSectionPreservingPosition();
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DIAGNOSTICS: {
      const int pageIndex = section ? section->currentPage : -1;
      const int pageCount = section ? section->pageCount : 0;
      startActivityForResult(
          std::make_unique<DiagnosticsActivity>(renderer, mappedInput, epub, currentSpineIndex, pageIndex, pageCount),
          [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::GENERATE_CACHE:
      // Cache building can take minutes on large books. Commit the current
      // reading interval before it and start a fresh one afterwards so it is
      // never included in the meter.
      READING_HISTORY.endSession();
      pregenerateCache();
      if (epub) {
        uint64_t bookId = 0;
        epub->getSourceFingerprint(&bookId);
        READING_HISTORY.beginSession(epub->getPath(), epub->getTitle(), epub->getAuthor(), bookId);
      }
      requestUpdate();
      break;
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      const bool hasProgress = epub && section;
      const uint16_t savedSpineIndex = hasProgress ? currentSpineIndex : 0;
      const uint16_t savedPage = hasProgress ? section->currentPage : 0;
      const uint16_t savedPageCount = hasProgress ? section->pageCount : 0;
      startActivityForResult(
          std::make_unique<BookCacheClearActivity>(renderer, mappedInput, epub),
          [this, hasProgress, savedSpineIndex, savedPage, savedPageCount](const ActivityResult& result) {
            if (!result.isCancelled) {
              section.reset();
              // progress.bin is deliberately restored after the cache directory is removed.
              // It is the only per-book state retained by this operation.
              if (hasProgress && epub) {
                epub->setupCacheDir();
                saveProgress(savedSpineIndex, savedPage, savedPageCount);
              }
              onGoHome();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TILT_PAGE_TURN:
      // Toggled inline in menu; IMU sync handled in result callback.
      break;
  }
}

void EpubReaderActivity::enterRubyAdjustMode() {
  rubyAdjustActive = true;
  rubyAdjustIgnoreOpeningRelease = true;
  rubyAdjustChanged = false;
  automaticPageTurnActive = false;
  requestUpdate();
}

void EpubReaderActivity::exitRubyAdjustMode() {
  rubyAdjustActive = false;
  rubyAdjustIgnoreOpeningRelease = false;
  const bool changed = rubyAdjustChanged;
  if (changed) {
    // Clean the moved ruby from the panel once before returning to normal
    // reader rendering.
    pagesUntilFullRefresh = 1;
  }
  rubyAdjustChanged = false;
  if (changed) {
    saveBookDirectionFields(BookReaderSettings::DirectionRubyOffsetX | BookReaderSettings::DirectionRubyOffsetY);
  }
  requestUpdate();
}

void EpubReaderActivity::adjustRubyOffset(const RubyAdjustAxis axis, const int delta) {
  auto& ds = SETTINGS.getDirectionSettings(verticalMode);
  uint8_t& target = axis == RubyAdjustAxis::X ? ds.rubyOffsetX : ds.rubyOffsetY;
  constexpr int maximum = 80;
  const int next = std::clamp(static_cast<int>(target) + delta, 0, maximum);
  if (next != target) {
    target = static_cast<uint8_t>(next);
    rubyAdjustChanged = true;
    requestUpdate();
  }
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    saveBookGlobalField(BookReaderSettings::Orientation);

    // Update renderer and input orientation to match the new coordinate system.
    OrientationHelper::applyOrientation(renderer, mappedInput, this);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= PAGE_TURN_LABELS.size()) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_LABELS[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  // A user turn outranks any saved resume/reflow position.
  {
    RenderLock lock(*this);
    clearDeferredReposition();
  }
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = UINT16_MAX;
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  // Resolve the writing mode before handling a restored end-of-book position.
  // A finished book has no section to load, but loop() still needs the correct
  // direction when mapping its end-screen buttons.
  if (!section) {
    if (SETTINGS.writingMode == CrossPointSettings::WM_VERTICAL) {
      verticalMode = true;
    } else if (SETTINGS.writingMode == CrossPointSettings::WM_HORIZONTAL) {
      verticalMode = false;
    } else {
      // Auto: check OPF hints
      verticalMode = epub && epub->isPageProgressionRtl() &&
                     (epub->getLanguage() == "ja" || epub->getLanguage() == "jpn" || epub->getLanguage() == "zh" ||
                      epub->getLanguage() == "zho");
    }
  }

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    saveProgress(currentSpineIndex, 0, 0, true);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  const auto& ds = SETTINGS.getDirectionSettings(verticalMode);

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += ds.screenMargin;
  orientedMarginLeft += ds.screenMargin;
  orientedMarginRight += ds.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(static_cast<int>(ds.screenMargin), getStatusBarContentReservation(statusBarHeight) +
                                                        UITheme::getInstance().getMetrics().statusBarVerticalMargin);
  } else {
    orientedMarginBottom +=
        std::max(static_cast<int>(ds.screenMargin), getStatusBarContentReservation(statusBarHeight));
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  if (!section) {
    // Ensure the correct SD card font is loaded for the resolved writing direction.
    // goToReader() calls ensureSdFontLoaded(false) before verticalMode is known,
    // so we reload here with the correct direction after resolution.
    ensureSdFontLoaded(verticalMode);

    // Load the OpenType 'vert' punctuation data while the reader has not yet
    // allocated page/render buffers. After a large file transfer the heap can
    // be fragmented enough that the former lazy load during drawTextVertical()
    // is skipped, leaving 、 and brackets on the horizontal fallback path.
    if (verticalMode) {
      if (auto* fcm = renderer.getFontCacheManager()) {
        fcm->clearCache();
        fcm->freeKernLigatureData();
      }
      renderer.ensureSdCardVerticalGlyphsReady(SETTINGS.getReaderFontId(verticalMode));
    }

    // ルビ用フォント: フォントロード後に8ptフォントを取得
    // rubyEnabled が OFF の場合は rubyFontId=0 でルビ描画をスキップ
    {
      const auto& rubyDs = SETTINGS.getDirectionSettings(verticalMode);

      LOG_INF("RUBY", "verticalMode=%d rubyEnabled=%d sdFontFamilyName=%s readerFontId=%d", verticalMode ? 1 : 0,
              rubyDs.rubyEnabled ? 1 : 0, rubyDs.sdFontFamilyName, SETTINGS.getReaderFontId(verticalMode));

      if (!rubyDs.rubyEnabled) {
        TextBlock::rubyFontId = 0;
        LOG_INF("RUBY", "ruby disabled: TextBlock::rubyFontId=0");
      } else {
        static constexpr uint8_t RUBY_FONT_SIZE_ENUM = 5;  // 8pt
        int rubyId = 0;

        if (rubyDs.sdFontFamilyName[0] != '\0' && SETTINGS.sdFontIdResolver) {
          rubyId = SETTINGS.sdFontIdResolver(SETTINGS.sdFontResolverCtx, rubyDs.sdFontFamilyName, RUBY_FONT_SIZE_ENUM);

          LOG_INF("RUBY", "ruby resolver result rubyId=%d family=%s sizeEnum=%u", rubyId, rubyDs.sdFontFamilyName,
                  RUBY_FONT_SIZE_ENUM);
        }

        if (rubyId == 0) {
          rubyId = SETTINGS.getReaderFontId(verticalMode);
        }

        TextBlock::rubyFontId = rubyId;

        LOG_INF("RUBY", "TextBlock::rubyFontId=%d", TextBlock::rubyFontId);
      }
    }

    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    const float lineCompression = SETTINGS.getReaderLineCompression(verticalMode);
    renderer.setVerticalCharSpacing(SETTINGS.getVerticalCharSpacingPercent());
    LOG_DBG("ERS", "Reflow params: lineSpacing=%u, compression=%.2f, viewport=%ux%u, vertical=%d", ds.lineSpacing,
            lineCompression, viewportWidth, viewportHeight, verticalMode);

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(verticalMode), lineCompression, ds.extraParagraphSpacing,
                                  ds.paragraphAlignment, viewportWidth, viewportHeight, ds.hyphenationEnabled,
                                  ds.firstLineIndent, SETTINGS.embeddedStyle, SETTINGS.imageRendering, verticalMode,
                                  ds.charSpacing)) {
      LOG_DBG("ERS", "Cache not found, building...");

      // Apply vertical character spacing for layout calculation
      renderer.setVerticalCharSpacing(SETTINGS.getVerticalCharSpacingPercent());

      // Free SD card font data before section building to maximize available heap.
      // clearCache() frees prewarm data (~130KB: miniGlyphs + miniBitmap).
      // freeKernLigatureData() frees kern/ligature tables (~22KB per style).
      // Both are lazy-loaded again during the render pass.
      auto* fcm = renderer.getFontCacheManager();
      if (fcm) {
        fcm->clearCache();
        fcm->freeKernLigatureData();
      }

      const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };

      const int headingFontIds[6] = {
          SETTINGS.getHeadingFontId(1, verticalMode), SETTINGS.getHeadingFontId(2, verticalMode), 0, 0, 0, 0};
      const int cssBodyFontIds[4] = {SETTINGS.getReaderFontIdForSize(verticalMode, CrossPointSettings::SMALL),
                                     SETTINGS.getReaderFontIdForSize(verticalMode, CrossPointSettings::MEDIUM),
                                     SETTINGS.getReaderFontIdForSize(verticalMode, CrossPointSettings::LARGE),
                                     SETTINGS.getReaderFontIdForSize(verticalMode, CrossPointSettings::EXTRA_LARGE)};

      if (!section->createSectionFile(SETTINGS.getReaderFontId(verticalMode), lineCompression, ds.extraParagraphSpacing,
                                      ds.paragraphAlignment, viewportWidth, viewportHeight, ds.hyphenationEnabled,
                                      ds.firstLineIndent, SETTINGS.embeddedStyle, SETTINGS.imageRendering, verticalMode,
                                      ds.charSpacing, popupFn, headingFontIds, SETTINGS.getTableFontId(verticalMode),
                                      cssBodyFontIds)) {
        LOG_ERR("ERS", "Failed to persist page data to SD (free heap: %d)", ESP.getFreeHeap());
        section.reset();
        // Show error and return to home to avoid infinite retry loop
        // (loop() would call requestUpdate() → render() → same failure)
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
        vTaskDelay(pdMS_TO_TICKS(2000));
        onGoHome();
        return;
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (nextPageNumber == UINT16_MAX) {
      section->currentPage = section->pageCount - 1;
    } else {
      section->currentPage = nextPageNumber;
    }

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      section->clearCache();
      section.reset();
      requestUpdate();  // Try again after clearing cache
                        // TODO: prevent infinite loop if the page keeps failing to load for some reason
      automaticPageTurnActive = false;
      return;
    }

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  {
    bool nearEnd = false;
    if (epub->getBookSize() > 0 && section->pageCount > 0) {
      const float chapterProgress =
          static_cast<float>(section->currentPage + 1) / static_cast<float>(section->pageCount);
      nearEnd = epub->calculateProgress(currentSpineIndex, chapterProgress) >= 0.95f;
    }
    saveProgress(currentSpineIndex, section->currentPage, section->pageCount, nearEnd);
  }

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  // Build the next chapter cache while the penultimate page is on screen.
  if (section->currentPage != section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  // Page turns can redraw the penultimate page several times in quick
  // succession.  Do not repeatedly start the same best-effort cache build.
  if (lastSilentIndexAttemptedSpineIndex == nextSpineIndex) {
    return;
  }

  // ZIP streaming and the parser both need a sizeable contiguous allocation.
  // This is speculative work, so defer it instead of fragmenting memory while
  // the reader is responding to page turns.
  constexpr uint32_t MIN_MAX_ALLOC_FOR_SILENT_INDEX = 30 * 1024;
  if (ESP.getMaxAllocHeap() < MIN_MAX_ALLOC_FOR_SILENT_INDEX) {
    LOG_DBG("ERS", "Skipping silent indexing for chapter %d (maxAlloc=%u, need >=%u)", nextSpineIndex,
            ESP.getMaxAllocHeap(), MIN_MAX_ALLOC_FOR_SILENT_INDEX);
    lastSilentIndexAttemptedSpineIndex = nextSpineIndex;
    return;
  }

  const auto& silentDs = SETTINGS.getDirectionSettings(verticalMode);
  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(SETTINGS.getReaderFontId(verticalMode),
                                  SETTINGS.getReaderLineCompression(verticalMode), silentDs.extraParagraphSpacing,
                                  silentDs.paragraphAlignment, viewportWidth, viewportHeight,
                                  silentDs.hyphenationEnabled, silentDs.firstLineIndent, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, verticalMode, silentDs.charSpacing)) {
    return;
  }

  lastSilentIndexAttemptedSpineIndex = nextSpineIndex;
  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  const int silentHeadingFontIds[6] = {
      SETTINGS.getHeadingFontId(1, verticalMode), SETTINGS.getHeadingFontId(2, verticalMode), 0, 0, 0, 0};
  const int cssBodyFontIds[4] = {SETTINGS.getReaderFontIdForSize(verticalMode, CrossPointSettings::SMALL),
                                 SETTINGS.getReaderFontIdForSize(verticalMode, CrossPointSettings::MEDIUM),
                                 SETTINGS.getReaderFontIdForSize(verticalMode, CrossPointSettings::LARGE),
                                 SETTINGS.getReaderFontIdForSize(verticalMode, CrossPointSettings::EXTRA_LARGE)};
  if (!nextSection.createSectionFile(SETTINGS.getReaderFontId(verticalMode),
                                     SETTINGS.getReaderLineCompression(verticalMode), silentDs.extraParagraphSpacing,
                                     silentDs.paragraphAlignment, viewportWidth, viewportHeight,
                                     silentDs.hyphenationEnabled, silentDs.firstLineIndent, SETTINGS.embeddedStyle,
                                     SETTINGS.imageRendering, verticalMode, silentDs.charSpacing, nullptr,
                                     silentHeadingFontIds, SETTINGS.getTableFontId(verticalMode), cssBodyFontIds)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount, bool isFinished) {
  uint8_t data[7];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = currentPage & 0xFF;
  data[3] = (currentPage >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  data[6] = isFinished ? 1 : 0;
  uint64_t bookId = 0;
  const bool hasBookId = epub->getSourceFingerprint(&bookId);
  const std::string progressPath = hasBookId ? BookDataPath::getProgressPath(bookId)
                                             : epub->getCachePath() + "/progress.bin";
  if ((!hasBookId || BookDataPath::ensureDirectory(bookId)) &&
      ProgressFile::writeAtomicPath(progressPath, data, sizeof(data))) {
    std::vector<BookListStatusEntry> statusEntries;
    loadBookListStatusIndex("/.crosspoint", statusEntries);
    updateBookListStatusIndex(epub->getPath(), isFinished ? ReadingStatus::Finished : ReadingStatus::Reading,
                              CachedBookStatus::Unknown, statusEntries);
    saveBookListStatusIndex("/.crosspoint", statusEntries);
    LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d, Finished: %d", spineIndex, currentPage, isFinished);
  } else {
    LOG_ERR("ERS", "Could not save progress!");
  }
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const int viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  const int readerFontId = SETTINGS.getReaderFontId(verticalMode);
  const auto& directionSettings = SETTINGS.getDirectionSettings(verticalMode);
  constexpr int horizontalRubyBaseShift = 10;
  const int rubyOffsetX = static_cast<int>(std::min<uint8_t>(directionSettings.rubyOffsetX, 80)) - 16 +
                          (verticalMode ? 0 : horizontalRubyBaseShift);
  const int rubyOffsetY = static_cast<int>(std::min<uint8_t>(directionSettings.rubyOffsetY, 80)) - 16;
  const auto t0 = millis();

  // Preload external font glyphs: collect codepoints from page, sort them,
  // and batch-read from SD sequentially. Much faster than random reads during render.
  FontManager& fm = FontManager::getInstance();
  if (fm.isExternalFontEnabled()) {
    ExternalFont* extFont = fm.getActiveFont();
    if (extFont) {
      std::vector<uint32_t> codepoints;
      page->collectCodepoints(codepoints, extFont->getCacheCapacity());
      if (!codepoints.empty()) {
        extFont->preloadGlyphs(codepoints.data(), codepoints.size());
      }
    }
  }

  // Apply vertical character spacing setting for this render
  renderer.setVerticalCharSpacing(SETTINGS.getVerticalCharSpacingPercent());

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  // Build the ruby advance table once for the whole page. Previously every
  // ruby token rebuilt it independently, repeatedly allocating and reading SD.
  if (TextBlock::rubyFontId != 0 && renderer.isSdCardFont(TextBlock::rubyFontId)) {
    std::string pageRubyText;
    page->collectRubyText(pageRubyText);
    if (!pageRubyText.empty()) {
      renderer.ensureSdCardFontReady(TextBlock::rubyFontId, pageRubyText.c_str(), 1u << EpdFontFamily::REGULAR);
    }
  }
  page->render(renderer, readerFontId, orientedMarginLeft, orientedMarginTop, viewportWidth, viewportHeight,
               rubyOffsetX, rubyOffsetY);  // scan pass
  // Include a CJK book/chapter title in the same prewarm pass.  This keeps the
  // status bar from faulting its compressed glyphs after the page is drawn.
  renderStatusBar();
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  page->render(renderer, readerFontId, orientedMarginLeft, orientedMarginTop, viewportWidth, viewportHeight,
               rubyOffsetX, rubyOffsetY);
  updateBookmarkFlag();
  renderStatusBar();
  renderRubyAdjustOverlay();
  if (bookmarkNotice != BookmarkNotice::NONE) {
    const char* message = tr(STR_BOOKMARK_ADDED);
    if (bookmarkNotice == BookmarkNotice::REMOVED) message = tr(STR_BOOKMARK_REMOVED);
    if (bookmarkNotice == BookmarkNotice::LIMIT) message = tr(STR_BOOKMARK_LIMIT);
    GUI.drawPopup(renderer, message);
  }
  const auto tBwRender = millis();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  bookmarkNotice = BookmarkNotice::NONE;
  const auto tDisplay = millis();

  // Illustration caches store four real pixel levels, but the normal BW pass
  // intentionally draws every non-white level as black. Re-render only images
  // into the two grayscale bit planes so text stays crisp and the panel can
  // display the cached dark/light gray values instead of Bayer dots alone.
  const bool hasImages = page->hasImages();
  bool bwStored = false;
  auto tGrayLsb = tDisplay;
  auto tGrayMsb = tDisplay;
  auto tGrayDisplay = tDisplay;
  auto tBwRestore = tDisplay;
  if (hasImages) {
    bwStored = renderer.storeBwBuffer();
    if (bwStored) {
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      page->renderImages(renderer, readerFontId, orientedMarginLeft, orientedMarginTop, viewportWidth);
      renderer.copyGrayscaleLsbBuffers();
      tGrayLsb = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      page->renderImages(renderer, readerFontId, orientedMarginLeft, orientedMarginTop, viewportWidth);
      renderer.copyGrayscaleMsbBuffers();
      tGrayMsb = millis();

      renderer.displayGrayBuffer();
      tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      tBwRestore = millis();

      // The grayscale image overlay can leave residual charge that a subsequent
      // FAST_REFRESH text page does not clear.  Keep the normal image rendering
      // path intact, but make the next ordinary page use the existing
      // HALF_REFRESH cleanup path once (CrossPoint Reader #2226).
      pagesUntilFullRefresh = 1;
    } else {
      LOG_ERR("ERS", "Failed to store BW buffer for illustration grayscale");
    }
  }

  const auto tEnd = millis();
  if (hasImages && bwStored) {
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
            "gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
            tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
  } else {
    LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
            tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
  }
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, verticalMode,
                    currentPageBookmarked);
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  currentPageBookmarked = false;
  if (!epub) return;
  const std::string legacyPath = BookmarkUtil::getBookmarkPath(epub->getPath());
  uint64_t bookId = 0;
  const bool hasBookId = epub->getSourceFingerprint(&bookId);
  const std::string path = hasBookId ? BookDataPath::getBookmarkPath(bookId) : legacyPath;
  BookmarkUtil::recoverBookmarkFile(path);
  if (Storage.exists(path.c_str())) {
    const String json = Storage.readFile(path.c_str());
    if (!json.isEmpty()) JsonSettingsIO::loadBookmarks(cachedBookmarks, json.c_str(), MAX_BOOKMARKS_PER_BOOK);
  } else if (hasBookId) {
    BookmarkUtil::recoverBookmarkFile(legacyPath);
    if (Storage.exists(legacyPath.c_str())) {
      const String json = Storage.readFile(legacyPath.c_str());
      if (!json.isEmpty() && JsonSettingsIO::loadBookmarks(cachedBookmarks, json.c_str(), MAX_BOOKMARKS_PER_BOOK) &&
          BookDataPath::ensureDirectory(bookId) && JsonSettingsIO::saveBookmarks(cachedBookmarks, path.c_str())) {
        LOG_INF("BKM", "Migrated bookmarks to BookId %016llx", static_cast<unsigned long long>(bookId));
      }
    }
  }
  updateBookmarkFlag();
}

void EpubReaderActivity::updateBookmarkFlag() {
  currentPageBookmarked = false;
  if (!epub || !section || cachedBookmarks.empty() || section->pageCount <= 0) return;
  const auto range = getBookmarkPageRange(epub, currentSpineIndex, section->currentPage, section->pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& entry) {
    if (entry.spineIndex == currentSpineIndex && entry.chapterPageCount == section->pageCount &&
        entry.chapterPage == section->currentPage)
      return true;
    return entry.percentage + BOOKMARK_PROGRESS_EPSILON >= range.start &&
           entry.percentage - BOOKMARK_PROGRESS_EPSILON <= range.end;
  });
}

void EpubReaderActivity::toggleBookmark() {
  if (!epub || !section || section->pageCount <= 0) return;
  const int page = section->currentPage;
  const int pageCount = section->pageCount;
  const auto range = getBookmarkPageRange(epub, currentSpineIndex, page, pageCount);
  const auto existing = std::find_if(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& entry) {
    if (entry.spineIndex == currentSpineIndex && entry.chapterPageCount == pageCount && entry.chapterPage == page)
      return true;
    return entry.percentage + BOOKMARK_PROGRESS_EPSILON >= range.start &&
           entry.percentage - BOOKMARK_PROGRESS_EPSILON <= range.end;
  });
  if (existing != cachedBookmarks.end()) {
    cachedBookmarks.erase(existing);
    bookmarkNotice = BookmarkNotice::REMOVED;
  } else if (cachedBookmarks.size() >= MAX_BOOKMARKS_PER_BOOK) {
    bookmarkNotice = BookmarkNotice::LIMIT;
  } else {
    BookmarkEntry entry;
    entry.spineIndex = static_cast<uint16_t>(currentSpineIndex);
    entry.chapterPageCount = static_cast<uint16_t>(pageCount);
    entry.chapterPage = static_cast<uint16_t>(page);
    const float chapterProgress = pageCount <= 1 ? 0.0f : static_cast<float>(page) / static_cast<float>(pageCount - 1);
    entry.percentage = epub->calculateProgress(currentSpineIndex, chapterProgress);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    entry.summary = tocIndex >= 0 ? epub->getTocItem(tocIndex).title : std::string(tr(STR_UNNAMED));
    cachedBookmarks.insert(cachedBookmarks.begin(), std::move(entry));
    bookmarkNotice = BookmarkNotice::ADDED;
  }
  uint64_t bookId = 0;
  const bool hasBookId = epub->getSourceFingerprint(&bookId);
  const std::string path = hasBookId ? BookDataPath::getBookmarkPath(bookId)
                                     : BookmarkUtil::getBookmarkPath(epub->getPath());
  if (!((!hasBookId || BookDataPath::ensureDirectory(bookId)) &&
        JsonSettingsIO::saveBookmarks(cachedBookmarks, path.c_str()))) {
    LOG_ERR("BKM", "Failed to save bookmarks");
  }
  updateBookmarkFlag();
  requestUpdate();
}

void EpubReaderActivity::renderRubyAdjustOverlay() const {
  if (!rubyAdjustActive) return;
  const auto& ds = SETTINGS.getDirectionSettings(verticalMode);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int footerHeight = metrics.buttonHintsHeight;
  const auto orientation = renderer.getOrientation();
  const bool isLandscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                           orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool frontHintsAtTop = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int footerY = frontHintsAtTop ? 0 : screenHeight - footerHeight;
  const int valueBandHeight = 26;
  const int valueY = frontHintsAtTop ? screenHeight - footerHeight - valueBandHeight + 5 : 5;

  // Keep the temporary controls legible over dense book text. The final ruby
  // position remains visible after Done removes this overlay.
  int valueBandX = 0;
  int valueBandWidth = screenWidth;
  int adjustedValueY = valueY;
  if (isLandscape) {
    constexpr int frontHintWidth = 100;
    constexpr int sideHintWidth = 54;
    const bool frontHintsOnLeft = orientation == GfxRenderer::Orientation::LandscapeClockwise;
    const int frontHintX = frontHintsOnLeft ? 0 : screenWidth - frontHintWidth;
    const int sideHintX = frontHintsOnLeft ? screenWidth - sideHintWidth : 0;
    renderer.fillRect(frontHintX, 0, frontHintWidth, screenHeight, false);
    renderer.fillRect(sideHintX, 0, sideHintWidth, screenHeight, false);
    valueBandX = frontHintsOnLeft ? frontHintWidth : sideHintWidth;
    valueBandWidth = screenWidth - frontHintWidth - sideHintWidth;
    adjustedValueY = 5;
  } else {
    renderer.fillRect(0, footerY, screenWidth, footerHeight, false);
    renderer.fillRect(screenWidth - metrics.sideButtonHintsWidth, 0, metrics.sideButtonHintsWidth, screenHeight, false);
  }
  renderer.fillRect(valueBandX, adjustedValueY - 5, valueBandWidth, valueBandHeight, false);

  char value[24];
  snprintf(value, sizeof(value), "X:%+d  Y:%+d", static_cast<int>(ds.rubyOffsetX) - 16,
           static_cast<int>(ds.rubyOffsetY) - 16);
  const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value);
  renderer.drawText(UI_10_FONT_ID, valueBandX + (valueBandWidth - valueWidth) / 2, adjustedValueY, value);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), tr(STR_RUBY_X_MINUS), tr(STR_RUBY_X_PLUS));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // With the right side up, the fixed physical side buttons appear in the
  // opposite screen order. Keep the hint order aligned with their actual Y action.
  const bool swapRubyYHints = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  GUI.drawSideButtonHints(renderer, swapRubyYHints ? tr(STR_RUBY_Y_PLUS) : tr(STR_RUBY_Y_MINUS),
                          swapRubyYHints ? tr(STR_RUBY_Y_MINUS) : tr(STR_RUBY_Y_PLUS));
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    clearDeferredReposition();
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    clearDeferredReposition();
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}
