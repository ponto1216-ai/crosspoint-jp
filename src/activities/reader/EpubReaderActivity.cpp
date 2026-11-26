#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/SinglePageCacheCompletion.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FontManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <ImageRenderBudget.h>
#include <ImageRenderDiagnostics.h>
#include <Issue18Diagnostics.h>
#include <Logging.h>
#include <SdFontDiagnostics.h>
#include <esp_system.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "BookCacheClearActivity.h"
#include "BookReaderSettings.h"
#include "BookReaderSettingsActivity.h"
#include "BookSyncActivity.h"
#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderDetailsActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "JsonSettingsIO.h"
#include "MappedInputManager.h"
#include "OrientationHelper.h"
#include "ProgressFile.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "ReadingHistoryStore.h"
#include "ReadingStatusHelper.h"
#include "RecentBooksStore.h"
#include "SdCardFontGlobals.h"
#include "activities/settings/DiagnosticsActivity.h"
#include "activities/settings/FontSelectionActivity.h"
#include "activities/settings/LineSpacingSelectionActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/settings/StatusBarSettingsActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookDataPath.h"
#include "util/BookmarkUtil.h"
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

uint8_t diagnosticPointSize(const uint8_t fontSize) {
  static constexpr uint8_t POINT_SIZES[] = {12, 14, 16, 18};
  return fontSize < sizeof(POINT_SIZES) ? POINT_SIZES[fontSize] : 0;
}

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

int pregeneratePixelCaches(const Page& page, GfxRenderer& renderer, const int xOffset, const int yOffset,
                           bool& framebufferInvalidated) {
  int generated = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageImage) continue;
    const auto& pageImage = static_cast<const PageImage&>(*element);
    const auto& image = pageImage.getImageBlock();
    if (image.pregeneratePixelCache(renderer, pageImage.xPos + xOffset, pageImage.yPos + yOffset,
                                    &framebufferInvalidated))
      generated++;
  }
  return generated;
}

}  // namespace

void EpubReaderActivity::pregenerateCache() {
  CacheGenerationControls controls(mappedInput);
  const uint32_t generationStartedAt = millis();
  uint32_t sectionBuildMs = 0;
  uint32_t pixelCacheMs = 0;
  int sectionCacheHits = 0;
  int generatedSections = 0;
  int generatedPixelCaches = 0;
  if (!epub) return;

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0) return;
  // Drop the completion state before work begins.  A reset/crash then resumes
  // through validated per-section files instead of treating a partial run as done.
  epub->clearFullCacheGeneratedMarker();

  bool isVertical = false;
  if (epub->isPlainText()) {
    isVertical = false;
  } else if (SETTINGS.writingMode == CrossPointSettings::WM_VERTICAL) {
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
  configureSmallFont(isVertical);

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
  const auto cancelLabels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, cancelLabels.btn1, cancelLabels.btn2, cancelLabels.btn3, cancelLabels.btn4);
  Rect popupRect = GUI.drawProgressPopup(renderer, tr(STR_GENERATING_CACHE), progressDetail.c_str());
  uint32_t progressDisplayMs = millis() - initialDisplayStartedAt;
  int lastDisplayedProgress = 0;
  bool framebufferInvalidated = false;
  const auto restoreProgress = [&] {
    if (!framebufferInvalidated) return;
    const uint32_t startedAt = millis();
    renderer.clearScreen();
    const int centerY = renderer.getScreenHeight() / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_GENERATING_CACHE));
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + 25, tr(STR_CACHE_CANCEL_HINT_LINE1));
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + 45, tr(STR_CACHE_CANCEL_HINT_LINE2));
    const auto cancelLabels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, cancelLabels.btn1, cancelLabels.btn2, cancelLabels.btn3, cancelLabels.btn4);
    popupRect = GUI.drawProgressPopup(renderer, tr(STR_GENERATING_CACHE), progressDetail.c_str());
    GUI.updateProgressPopup(renderer, popupRect, progressDetail.c_str(), lastDisplayedProgress);
    progressDisplayMs += millis() - startedAt;
    framebufferInvalidated = false;
    LOG_DBG("IMEM", "Restored progress UI after JPEG framebuffer loan");
  };

  bool cancelled = false;

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
    const bool sectionCached =
        sec.loadSectionFile(SETTINGS.getReaderFontId(isVertical), SETTINGS.getTableFontId(isVertical), lineCompression,
                            ds.extraParagraphSpacing, ds.paragraphAlignment, viewportWidth, viewportHeight,
                            ds.hyphenationEnabled, ds.firstLineIndent, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                            isVertical, ds.charSpacing, ds.tateChuYokoMaxDigits);
    if (sectionCached) {
      sectionCacheHits++;
    } else {
      const uint32_t sectionStartedAt = millis();
      bool cancelledDuringSection = false;
      const int cssBodyFontIds[4] = {SETTINGS.getReaderFontIdForSize(isVertical, CrossPointSettings::SMALL),
                                     SETTINGS.getReaderFontIdForSize(isVertical, CrossPointSettings::MEDIUM),
                                     SETTINGS.getReaderFontIdForSize(isVertical, CrossPointSettings::LARGE),
                                     SETTINGS.getReaderFontIdForSize(isVertical, CrossPointSettings::EXTRA_LARGE)};
      const bool sectionCreated = sec.createSectionFile(
          SETTINGS.getReaderFontId(isVertical), lineCompression, ds.extraParagraphSpacing, ds.paragraphAlignment,
          viewportWidth, viewportHeight, ds.hyphenationEnabled, ds.firstLineIndent, SETTINGS.embeddedStyle,
          SETTINGS.imageRendering, isVertical, ds.charSpacing, ds.tateChuYokoMaxDigits, nullptr, headingFontIds,
          SETTINGS.getTableFontId(isVertical), cssBodyFontIds, nullptr,
          [this, &generatedPixelCaches, &pixelCacheMs, &framebufferInvalidated, &restoreProgress, orientedMarginLeft,
           orientedMarginTop](const Page& page) {
            const uint32_t pixelStartedAt = millis();
            generatedPixelCaches +=
                pregeneratePixelCaches(page, renderer, orientedMarginLeft, orientedMarginTop, framebufferInvalidated);
            pixelCacheMs += millis() - pixelStartedAt;
            restoreProgress();
          },
          [&cancelledDuringSection, &controls, this] {
            cancelledDuringSection = controls.shouldCancel(renderer);
            return cancelledDuringSection;
          });
      if (!sectionCreated) {
        if (cancelledDuringSection) {
          LOG_DBG("ERS", "Pregenerate cancelled while building section %d/%d", i, spineCount);
          cancelled = true;
          break;
        }
        LOG_ERR("ERS", "Pregenerate: failed section %d (heap: %d)", i, ESP.getFreeHeap());
      } else {
        sectionBuildMs += millis() - sectionStartedAt;
        generatedSections++;
      }
    }
    // A full-book run must not carry an earlier chapter's SD-font advance
    // tables into the next one. They reload lazily for the next layout or
    // render, while releasing them here restores a contiguous heap block.
    if (fcm) {
      fcm->releaseSdFontCaches();
      fcm->releaseSdFontVerticalGlyphs();
    }
  }
  if (cancelled) {
    GUI.updateProgressPopup(renderer, popupRect, tr(STR_CACHE_INTERRUPTED), 0);
  }
  const bool imagesComplete = !cancelled;

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
#if defined(SINGLE_CACHE_PROFILE)
  LOG_INF("SCP", "run total_ms=%lu section_ms=%lu generated=%d cached=%d image_ms=%lu images=%d progress_ms=%lu",
          millis() - generationStartedAt, sectionBuildMs, generatedSections, sectionCacheHits, pixelCacheMs,
          generatedPixelCaches, progressDisplayMs);
#endif
  LOG_DBG("ERS",
          "Pregenerate timing: total=%lu ms, section-build=%lu ms (%d generated, %d cached), PXC=%lu ms (%d "
          "images), progress=%lu ms",
          millis() - generationStartedAt, sectionBuildMs, generatedSections, sectionCacheHits, pixelCacheMs,
          generatedPixelCaches, progressDisplayMs);
}

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  Issue18Diagnostics::logMemory("reader-enter", epub->getPath().c_str());
  ReaderResumeState::clearRememberedBook();

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
  if (!yomuka::sync::recoverFile(progressPath)) LOG_ERR("ERS", "Could not recover saved progress");
  if (Storage.exists(progressPath.c_str())) {
    dataSize = ProgressFile::readLegacyCompatible(progressPath, data);
  } else if (hasBookId) {
    dataSize = ProgressFile::readLegacyCompatible(legacyProgressPath, data);
    uint8_t completeRecord[8] = {};
    size_t completeLength = 0;
    yomuka::sync::Progress checkedProgress;
    if (dataSize != 0 &&
        yomuka::sync::readBytes(legacyProgressPath, completeRecord, sizeof(completeRecord), completeLength) ==
            yomuka::sync::ReadStatus::Present &&
        yomuka::sync::decodeLegacyProgress(completeRecord, completeLength, checkedProgress) ==
            yomuka::sync::ProgressError::None &&
        BookDataPath::ensureDirectory(bookId) &&
        ProgressFile::writeAtomicPath(progressPath, completeRecord, completeLength, false)) {
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

void EpubReaderActivity::rememberBookOnceRendered() {
  if (!epub || !resumeState.takeRenderedBook()) return;
  uint64_t bookId = 0;
  epub->getSourceFingerprint(&bookId);
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath(), bookId);
}

void EpubReaderActivity::onExit() {
  // Also commit when sleep/back arrives before the next reader loop.
  rememberBookOnceRendered();
  Activity::onExit();
#if defined(IDLE_CHAPTER_BUILD)
  // ActivityManager calls onExit while holding the non-recursive RenderLock.
  // Taking it again here would deadlock even when no build is active.
  idleChapter.reset();
  chapterRenderReady.store(0);
#endif
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
  rememberBookOnceRendered();
  if (CacheGenerationControls::consumeCancellationRelease(mappedInput)) return;
#if defined(IDLE_CHAPTER_BUILD)
  if (mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased()) {
    chapterLastInput = millis();
    // Never stall GPIO polling on a press edge: waiting for a full refresh can
    // turn a short press into a measured long press. If rendering owns the
    // mutex, render()/onExit() already discard the speculative state.
    RenderLock lock(RenderLock::TryLock::Now);
    if (lock.ownsLock()) {
      if (idleChapter) LOG_INF("NCH", "cancel input");
      idleChapter.reset();
    }
  }
#endif
#if defined(IDLE_IMAGE_PREFETCH_TEST)
  if (mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased()) idleLastInput = millis();
#endif
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
            [this] { saveBookGlobalField(BookReaderSettings::InvertImages); },
            [this] { saveBookDirectionFields(BookReaderSettings::DirectionFontSize); }),
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
  // MappedInputManager rotates the side controls with the device. Keep that
  // physical direction in landscape; only the CCW front controls need a
  // reading-direction correction.
  const bool reverseFrontButtons = verticalMode && orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput, reverseFrontButtons);
  (void)fromTilt;
  if (!prevTriggered && !nextTriggered) {
#if defined(IDLE_IMAGE_PREFETCH_TEST)
    prefetchIdleImage();
#endif
#if defined(IDLE_CHAPTER_BUILD)
    prefetchIdleChapter();
#endif
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
#if defined(IDLE_CHAPTER_DIAGNOSTICS)
  LOG_INF("NCH", "turn held=%lu skip=%d prev=%d next=%d", mappedInput.getHeldTime(), skipChapter, prevTriggered,
          nextTriggered);
#endif

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

void EpubReaderActivity::clearDeferredReposition() { cachedChapterTotalPageCount = 0; }

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SD_SYNC: {
      bool saved = true;
      {
        RenderLock lock(*this);
        if (section) {
          const int percent = calculateBookPercent(section->currentPage, section->pageCount);
          saved = saveProgress(currentSpineIndex, section->currentPage, section->pageCount, percent >= 95, percent);
        }
      }
      if (!saved || !READING_HISTORY.endSession()) {
        RenderLock lock(*this);
        GUI.drawPopup(renderer, "保存失敗。SDを確認してください");
        renderer.displayBuffer();
        return;
      }
      automaticPageTurnActive = false;
      activityManager.replaceActivity(std::make_unique<BookSyncActivity>(renderer, mappedInput, epub));
      return;
    }
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
      startActivityForResult(
          std::make_unique<BookReaderSettingsActivity>(renderer, mappedInput, fingerprint, verticalMode),
          [this, fingerprint](const ActivityResult& result) {
            if (result.isCancelled) return;
            BookReaderSettings::Override bookOverride;
            const bool hasOverride =
                BookReaderSettings::load(fingerprint, bookOverride) && BookReaderSettings::hasAnyField(bookOverride);
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
      const bool savedFinished = epub && currentSpineIndex == epub->getSpineItemsCount();
      const bool hasProgress = epub && (section || savedFinished);
      const uint16_t savedSpineIndex = hasProgress ? currentSpineIndex : 0;
      const uint16_t savedPage = section && !savedFinished ? section->currentPage : 0;
      const uint16_t savedPageCount = section && !savedFinished ? section->pageCount : 0;
      const int savedPercent = savedFinished ? 100 : calculateBookPercent(savedPage, savedPageCount);
      startActivityForResult(
          std::make_unique<BookCacheClearActivity>(renderer, mappedInput, epub),
          [this, hasProgress, savedFinished, savedSpineIndex, savedPage, savedPageCount,
           savedPercent](const ActivityResult& result) {
            if (!result.isCancelled) {
              section.reset();
              // progress.bin is deliberately restored after the cache directory is removed.
              // It is the only per-book state retained by this operation.
              if (hasProgress && epub) {
                epub->setupCacheDir();
                saveProgress(savedSpineIndex, savedPage, savedPageCount, savedFinished, savedPercent);
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
#if defined(IDLE_CHAPTER_BUILD)
  chapterRenderReady.store(0);
  idleChapter.reset();
#endif
#if defined(IDLE_IMAGE_PREFETCH_TEST)
  idleRenderReady.store(0);
#endif
  if (!epub) {
    return;
  }

  // Resolve the writing mode before handling a restored end-of-book position.
  // A finished book has no section to load, but loop() still needs the correct
  // direction when mapping its end-screen buttons.
  if (!section) {
    if (epub->isPlainText()) {
      verticalMode = false;
    } else if (SETTINGS.writingMode == CrossPointSettings::WM_VERTICAL) {
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
    saveProgress(currentSpineIndex, 0, 0, true, 100);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    resumeState.markPageRendered();
    automaticPageTurnActive = false;
    return;
  }

  const auto& ds = SETTINGS.getDirectionSettings(verticalMode);
  SD_FONT_DIAG_CONTEXT(ds.sdFontFamilyName, diagnosticPointSize(ds.fontSize), currentSpineIndex, nextPageNumber);

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
    // Native USB disconnects during reboot. Replay the captured boot-time font
    // load sample after the monitor has reconnected and the book is opened.
    SD_FONT_DIAG_REPLAY_FONT_LOAD();

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

    // Script text uses the already-supported smaller companion font. Unlike
    // ruby, this remains active when ruby display is disabled.
    configureSmallFont(verticalMode);
    LOG_INF("SCRIPT", "TextBlock::smallFontId=%d", TextBlock::smallFontId);

    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    const float lineCompression = SETTINGS.getReaderLineCompression(verticalMode);
    renderer.setVerticalCharSpacing(SETTINGS.getVerticalCharSpacingPercent());
    LOG_DBG("ERS", "Reflow params: lineSpacing=%u, compression=%.2f, viewport=%ux%u, vertical=%d", ds.lineSpacing,
            lineCompression, viewportWidth, viewportHeight, verticalMode);

    SD_FONT_DIAG_LOG("page_layout_check_before", 0);
    if (!section->loadSectionFile(SETTINGS.getReaderFontId(verticalMode), SETTINGS.getTableFontId(verticalMode),
                                  lineCompression, ds.extraParagraphSpacing, ds.paragraphAlignment, viewportWidth,
                                  viewportHeight, ds.hyphenationEnabled, ds.firstLineIndent, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, verticalMode, ds.charSpacing, ds.tateChuYokoMaxDigits)) {
      LOG_DBG("ERS", "Cache not found, building...");
      const uint32_t pageLayoutStartedAt = SD_FONT_DIAG_NOW_US();
      SD_FONT_DIAG_LOG("page_layout_before", 0);

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
        SD_FONT_DIAG_LOG("page_transition_release_after", 0);
      }

      const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };

      const int headingFontIds[6] = {
          SETTINGS.getHeadingFontId(1, verticalMode), SETTINGS.getHeadingFontId(2, verticalMode), 0, 0, 0, 0};
      const int cssBodyFontIds[4] = {SETTINGS.getReaderFontIdForSize(verticalMode, CrossPointSettings::SMALL),
                                     SETTINGS.getReaderFontIdForSize(verticalMode, CrossPointSettings::MEDIUM),
                                     SETTINGS.getReaderFontIdForSize(verticalMode, CrossPointSettings::LARGE),
                                     SETTINGS.getReaderFontIdForSize(verticalMode, CrossPointSettings::EXTRA_LARGE)};

      bool sectionCreated = section->createSectionFile(
          SETTINGS.getReaderFontId(verticalMode), lineCompression, ds.extraParagraphSpacing, ds.paragraphAlignment,
          viewportWidth, viewportHeight, ds.hyphenationEnabled, ds.firstLineIndent, SETTINGS.embeddedStyle,
          SETTINGS.imageRendering, verticalMode, ds.charSpacing, ds.tateChuYokoMaxDigits, popupFn, headingFontIds,
          SETTINGS.getTableFontId(verticalMode), cssBodyFontIds);
      SD_FONT_DIAG_LOG_AFTER(sectionCreated ? "page_layout_after" : "page_layout_failed", 0, pageLayoutStartedAt);
      // Wi-Fi teardown after a Web UI transfer completes asynchronously. If it
      // left the largest heap block below the ZIP-stream requirement, yield
      // once and retry instead of forcing the user to restart the device.
      constexpr uint32_t SECTION_STREAM_MIN_CONTIGUOUS_HEAP = 32 * 1024;
      if (!sectionCreated && ESP.getMaxAllocHeap() < SECTION_STREAM_MIN_CONTIGUOUS_HEAP) {
        if (fcm) {
          fcm->releaseSdFontCaches();
          fcm->releaseSdFontVerticalGlyphs();
        }
        vTaskDelay(pdMS_TO_TICKS(250));
        LOG_INF("ERS", "Retrying section build after heap recovery (free=%u, maxAlloc=%u)", ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());
        sectionCreated = section->createSectionFile(
            SETTINGS.getReaderFontId(verticalMode), lineCompression, ds.extraParagraphSpacing, ds.paragraphAlignment,
            viewportWidth, viewportHeight, ds.hyphenationEnabled, ds.firstLineIndent, SETTINGS.embeddedStyle,
            SETTINGS.imageRendering, verticalMode, ds.charSpacing, ds.tateChuYokoMaxDigits, popupFn, headingFontIds,
            SETTINGS.getTableFontId(verticalMode), cssBodyFontIds);
      }

      if (!sectionCreated) {
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
      SD_FONT_DIAG_LOG("page_layout_cached", 0);
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

    // Only a fresh, fully persisted single-page text build proves this whole book is ready.
    if (singlepagecache::eligible(epub->getSpineItemsCount(), currentSpineIndex, section->pageCount,
                                  section->hasFreshTextOnlyBuild(), p->hasImages()) &&
        !epub->isFullCacheGenerated()) {
      if (epub->markFullCacheGenerated()) {
        LOG_INF("ERS", "Single-page text cache complete");
      } else {
        LOG_ERR("ERS", "Could not publish single-page completion marker");
      }
    }

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    const uint32_t pageDrawStartedAt = SD_FONT_DIAG_NOW_US();
    SD_FONT_DIAG_CONTEXT(ds.sdFontFamilyName, diagnosticPointSize(ds.fontSize), currentSpineIndex,
                         section->currentPage);
    SD_FONT_DIAG_LOG("page_draw_before", 0);
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    resumeState.markPageRendered();
    SD_FONT_DIAG_LOG_AFTER("page_draw_after", 0, pageDrawStartedAt);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
#if !defined(IDLE_CHAPTER_BUILD)
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
#else
  chapterViewportWidth = viewportWidth;
  chapterViewportHeight = viewportHeight;
#endif
  {
    const int percent = calculateBookPercent(section->currentPage, section->pageCount);
    saveProgress(currentSpineIndex, section->currentPage, section->pageCount, percent >= 95, percent);
  }

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
#if defined(IDLE_IMAGE_PREFETCH_TEST)
  idleRenderEpoch.fetch_add(1);
  idleRenderReady.store(millis());
#endif
#if defined(IDLE_CHAPTER_BUILD)
#if defined(IDLE_CHAPTER_DIAGNOSTICS)
  LOG_INF("NCH", "view spine=%d page=%d/%u", currentSpineIndex, section->currentPage + 1, section->pageCount);
#endif
  chapterRenderReady.store(millis());
#endif
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
#if defined(IDLE_CHAPTER_BUILD)
    LOG_INF("NCH", "skip heap spine=%d max=%u", nextSpineIndex, ESP.getMaxAllocHeap());
#endif
    return;
  }

  const auto& silentDs = SETTINGS.getDirectionSettings(verticalMode);
#if defined(IDLE_CHAPTER_BUILD)
  idleChapter.reset(new (std::nothrow) Section(epub, nextSpineIndex, renderer));
  if (!idleChapter) return;
  Section& nextSection = *idleChapter;
#else
  Section nextSection(epub, nextSpineIndex, renderer);
#endif
  if (nextSection.loadSectionFile(
          SETTINGS.getReaderFontId(verticalMode), SETTINGS.getTableFontId(verticalMode),
          SETTINGS.getReaderLineCompression(verticalMode), silentDs.extraParagraphSpacing, silentDs.paragraphAlignment,
          viewportWidth, viewportHeight, silentDs.hyphenationEnabled, silentDs.firstLineIndent, SETTINGS.embeddedStyle,
          SETTINGS.imageRendering, verticalMode, silentDs.charSpacing, silentDs.tateChuYokoMaxDigits)) {
#if defined(IDLE_CHAPTER_BUILD)
    lastSilentIndexAttemptedSpineIndex = nextSpineIndex;
    LOG_INF("NCH", "cache hit spine=%d", nextSpineIndex);
    idleChapter.reset();
#endif
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
  if (!nextSection.createSectionFile(
          SETTINGS.getReaderFontId(verticalMode), SETTINGS.getReaderLineCompression(verticalMode),
          silentDs.extraParagraphSpacing, silentDs.paragraphAlignment, viewportWidth, viewportHeight,
          silentDs.hyphenationEnabled, silentDs.firstLineIndent, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
          verticalMode, silentDs.charSpacing, silentDs.tateChuYokoMaxDigits, nullptr, silentHeadingFontIds,
          SETTINGS.getTableFontId(verticalMode), cssBodyFontIds
#if defined(IDLE_CHAPTER_BUILD)
          ,
          nullptr, nullptr,
          [this]() {
            chapterCancelled = chapterCancelled || chapterRenderReady.load() == 0 || gpio.pollIdleInput();
            return chapterCancelled;
          },
          true, true
#endif
          )) {
    LOG_DBG("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
#if defined(IDLE_CHAPTER_BUILD)
    LOG_INF("NCH", "prepare failed spine=%d reason=%d cancelled=%d", nextSpineIndex,
            static_cast<int>(nextSection.getLastCreateFailureReason()), chapterCancelled);
    idleChapter.reset();
#endif
  }
}

#if defined(IDLE_CHAPTER_BUILD)
void EpubReaderActivity::prefetchIdleChapter() {
  const uint32_t ready = chapterRenderReady.load();
  if (!ready || millis() - ready < 1500 || millis() - chapterLastInput < 1500 || automaticPageTurnActive ||
      rubyAdjustActive || SETTINGS.tiltPageTurn)
    return;
  for (uint8_t b = 0; b <= HalGPIO::BTN_POWER; ++b)
    if (gpio.isPressed(b)) return;
  RenderLock lock(RenderLock::TryLock::Now);
  if (!lock.ownsLock()) return;
  if (chapterRenderReady.load() != ready || !epub || !section) return;
  HalPowerManager::Lock powerLock;
  if (gpio.pollIdleInput()) {
    idleChapter.reset();
    chapterLastInput = millis();
    return;
  }
  const uint32_t started = millis();
  if (!idleChapter) {
#if defined(IDLE_CHAPTER_CANCEL_WINDOW_MS)
    chapterTestPause.reset();
#endif
    chapterCancelled = false;
    silentIndexNextChapterIfNeeded(chapterViewportWidth, chapterViewportHeight);
    if (idleChapter)
      LOG_INF("NCH", "prepared spine=%d ms=%lu free=%u max=%u", currentSpineIndex + 1, millis() - started,
              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    if (chapterCancelled) {
      idleChapter.reset();
      chapterLastInput = millis();
    }
    return;
  }
#if defined(IDLE_CHAPTER_CANCEL_WINDOW_MS)
  // Return to the normal loop so input processing remains live during the window.
  if (chapterTestPause.waiting(millis(), IDLE_CHAPTER_CANCEL_WINDOW_MS)) return;
#endif
  const auto result = idleChapter->stepIncrementalBuild();
#if defined(IDLE_CHAPTER_CANCEL_WINDOW_MS)
  if (result == Section::BuildStep::Pending && !chapterCancelled &&
      chapterTestPause.arm(millis(), idleChapter->pageCount)) {
    LOG_INF("NCH", "cancel test window=%u ms pages=%u", static_cast<unsigned>(IDLE_CHAPTER_CANCEL_WINDOW_MS),
            idleChapter->pageCount);
  }
#endif
#if defined(IDLE_CHAPTER_DIAGNOSTICS)
  const uint32_t elapsed = millis() - started;
  if (elapsed > 25 || result != Section::BuildStep::Pending)
    LOG_INF("NCH", "step result=%d ms=%lu pages=%u cancel=%d", static_cast<int>(result), elapsed,
            idleChapter->pageCount, chapterCancelled);
#else
  if (result != Section::BuildStep::Pending)
    LOG_INF("NCH", "finished spine=%d result=%d pages=%u cancel=%d", currentSpineIndex + 1, static_cast<int>(result),
            idleChapter->pageCount, chapterCancelled);
#endif
  if (result != Section::BuildStep::Pending || chapterCancelled) idleChapter.reset();
  if (chapterCancelled) chapterLastInput = millis();
}
#endif

int EpubReaderActivity::calculateBookPercent(const int currentPage, const int pageCount) const {
  if (!epub || epub->getBookSize() == 0 || pageCount <= 0) return -1;
  const float chapterProgress = static_cast<float>(currentPage + 1) / static_cast<float>(pageCount);
  return clampPercent(static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f));
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount, bool isFinished, int percent) {
  uint8_t data[8];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = currentPage & 0xFF;
  data[3] = (currentPage >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  data[6] = isFinished ? 1 : 0;
  data[7] = percent >= 0 && percent <= 100 ? static_cast<uint8_t>(percent) : ReadingProgress::PERCENT_UNKNOWN;
  uint64_t bookId = 0;
  const bool hasBookId = epub->getSourceFingerprint(&bookId);
  const std::string progressPath =
      hasBookId ? BookDataPath::getProgressPath(bookId) : epub->getCachePath() + "/progress.bin";
  if ((!hasBookId || BookDataPath::ensureDirectory(bookId)) &&
      ProgressFile::writeAtomicPath(progressPath, data, sizeof(data))) {
    std::vector<BookListStatusEntry> statusEntries;
    loadBookListStatusIndex("/.crosspoint", statusEntries);
    updateBookListStatusIndex(epub->getPath(), isFinished ? ReadingStatus::Finished : ReadingStatus::Reading,
                              CachedBookStatus::Unknown, statusEntries);
    saveBookListStatusIndex("/.crosspoint", statusEntries);
    if (isFinished) READING_HISTORY.markFinished(epub->getPath(), bookId);
    LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d, Finished: %d", spineIndex, currentPage, isFinished);
    return true;
  } else {
    LOG_ERR("ERS", "Could not save progress!");
    return false;
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
#if defined(IMAGE_RENDER_MEMORY_DIAGNOSTICS)
  imagerenderdiag::PageScope imageMemory(page->hasImages(), currentSpineIndex, section->currentPage);
#endif

  // Section generation may release optional vertical substitution data to
  // recover the contiguous ZIP-stream buffer on ESP32-C3. Load it only once
  // the cache is complete and this page is about to be drawn.
  if (verticalMode) {
    renderer.ensureSdCardVerticalGlyphsReady(readerFontId);
  }

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
  const auto tScanStart = millis();
  page->render(renderer, readerFontId, orientedMarginLeft, orientedMarginTop, viewportWidth, viewportHeight,
               rubyOffsetX, rubyOffsetY);  // scan pass
  // Include a CJK book/chapter title in the same prewarm pass.  This keeps the
  // status bar from faulting its compressed glyphs after the page is drawn.
  renderStatusBar();
  const auto tScanEnd = millis();
  SD_FONT_DIAG_LOG("glyph_scan_before_prewarm", 0);
  scope.endScanAndPrewarm();
  SD_FONT_DIAG_LOG("glyph_scan_after_prewarm", 0);
#if defined(RENDER_PROFILE)
  fcm->logStats("page");
#endif
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
  imagerenderdiag::mark("bw-render-after");
  const auto tBwRender = millis();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  bookmarkNotice = BookmarkNotice::NONE;
  const auto tDisplay = millis();
  imagerenderdiag::mark("bw-display-after");

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
    // Text and UI pixels are already drawn. Only images are rendered below,
    // so optional font caches may be reclaimed without changing this page.
    imagerenderdiag::mark("font-budget-before");
    const auto heapBefore = std::make_pair(ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    const unsigned released = imagerenderbudget::recover(
        renderer.getBufferSize(), fcm, [] { return std::make_pair(ESP.getFreeHeap(), ESP.getMaxAllocHeap()); },
        [](unsigned stage) { imagerenderdiag::mark(stage == 1 ? "font-glyphs-released" : "font-tables-released"); });
    const auto heapAfter = std::make_pair(ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    const bool low = imagerenderbudget::needsRecovery(renderer.getBufferSize(), heapAfter.first, heapAfter.second);
    LOG_DBG("IRB", "release=%u free=%u->%u maxAlloc=%u->%u frame=%u extra=%u low=%d", released,
            (unsigned)heapBefore.first, (unsigned)heapAfter.first, (unsigned)heapBefore.second,
            (unsigned)heapAfter.second, (unsigned)renderer.getBufferSize(), (unsigned)imagerenderbudget::EXTRA_BYTES,
            low);
    imagerenderdiag::mark("font-budget-after", released);
    // The estimate cannot guarantee all chunks fit; preserve the existing
    // checked allocation/failure path even when recovery cannot meet it.
    bwStored = renderer.storeBwBuffer();
    if (bwStored) {
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      page->renderImages(renderer, readerFontId, orientedMarginLeft, orientedMarginTop, viewportWidth);
      imagerenderdiag::mark("lsb-render-after");
      renderer.copyGrayscaleLsbBuffers();
      imagerenderdiag::mark("lsb-copy-after");
      tGrayLsb = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      page->renderImages(renderer, readerFontId, orientedMarginLeft, orientedMarginTop, viewportWidth);
      imagerenderdiag::mark("msb-render-after");
      renderer.copyGrayscaleMsbBuffers();
      imagerenderdiag::mark("msb-copy-after");
      tGrayMsb = millis();

      renderer.displayGrayBuffer();
      imagerenderdiag::mark("gray-display-after");
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
#if defined(RENDER_PROFILE)
#define LOG_RENDER_TIMING LOG_INF
#else
#define LOG_RENDER_TIMING LOG_DBG
#endif
  if (hasImages && bwStored) {
    LOG_RENDER_TIMING(
        "ERS",
        "Page render: prewarm=%lums prep=%lums scan=%lums cache=%lums bw_render=%lums display=%lums gray_lsb=%lums "
        "gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
        tPrewarm - t0, tScanStart - t0, tScanEnd - tScanStart, tPrewarm - tScanEnd, tBwRender - tPrewarm,
        tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb,
        tBwRestore - tGrayDisplay, tEnd - t0);
  } else {
    LOG_RENDER_TIMING(
        "ERS", "Page render: prewarm=%lums prep=%lums scan=%lums cache=%lums bw_render=%lums display=%lums total=%lums",
        tPrewarm - t0, tScanStart - t0, tScanEnd - tScanStart, tPrewarm - tScanEnd, tBwRender - tPrewarm,
        tDisplay - tBwRender, tEnd - t0);
  }
#undef LOG_RENDER_TIMING
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
    title = tr(STR_AUTO_TURN_ENABLED);
    if (I18N.getLanguage() == Language::JAPANESE) {
      title += std::to_string(pageTurnDuration / 1000) + "秒ごと";
    } else {
      title += std::to_string(60 * 1000 / pageTurnDuration);
    }

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
          BookDataPath::ensureDirectory(bookId) &&
          JsonSettingsIO::saveBookmarks(cachedBookmarks, path.c_str(), false)) {
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
  const std::string path =
      hasBookId ? BookDataPath::getBookmarkPath(bookId) : BookmarkUtil::getBookmarkPath(epub->getPath());
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

#if defined(IDLE_IMAGE_PREFETCH_TEST)
void EpubReaderActivity::prefetchIdleImage() {
  const uint32_t ready = idleRenderReady.load();
  if (!ready || millis() - ready < 1000 || millis() - idleLastInput < 1000 || automaticPageTurnActive ||
      rubyAdjustActive || SETTINGS.tiltPageTurn)
    return;
  for (uint8_t b = 0; b <= HalGPIO::BTN_POWER; ++b)
    if (gpio.isPressed(b)) return;
  RenderLock lock(RenderLock::TryLock::Now);
  if (!lock.ownsLock()) return;
  if (idleRenderReady.load() != ready || !epub || !section || !renderer.hasFrameBuffer()) return;
#if defined(IDLE_CHAPTER_BUILD)
  if (idleChapter) return;
#endif
  const uint32_t epoch = idleRenderEpoch.load();
  if (idleSeenEpoch != epoch) {
    idleSeenEpoch = epoch;
    idleElement = 0;
    idlePageDone = false;
  }
  if (idlePageDone) return;
  const int next = section->currentPage + 1;
  if (next >= section->pageCount) {
    idlePageDone = true;
    return;
  }
  // Do not evict fonts or borrow the framebuffer for speculative work.
  if (ESP.getFreeHeap() < 72 * 1024 || ESP.getMaxAllocHeap() < 20 * 1024) {
    idlePageDone = true;
    LOG_DBG("IPF", "Skipped low heap free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return;
  }
  HalPowerManager::Lock powerLock;
  if (gpio.pollIdleInput()) {
    idleLastInput = millis();
    return;
  }
  auto page = section->loadPageFromSectionFile(next);
  if (!page) {
    idlePageDone = true;
    return;
  }
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const auto margin = SETTINGS.getDirectionSettings(verticalMode).screenMargin;
  top += margin;
  left += margin;
  for (; idleElement < page->elements.size();) {
    const auto& e = page->elements[idleElement++];
    if (e->getTag() != TAG_PageImage) continue;
    const auto& image = static_cast<const PageImage&>(*e);
    uint32_t lastPoll = 0;
    DecodeCancellation cancel;
    cancel.context = &lastPoll;
    cancel.requested = [](void* context) {
      auto& last = *static_cast<uint32_t*>(context);
      if (millis() - last < 5) return false;
      last = millis();
      return gpio.pollIdleInput();
    };
    const uint32_t started = millis();
    LOG_DBG("IPF", "Start spine=%d next=%d element=%u free=%u maxAlloc=%u", currentSpineIndex, next,
            (unsigned)(idleElement - 1), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    const auto result = image.getImageBlock().ensurePixelCache(renderer, image.xPos + left, image.yPos + top, false,
                                                               nullptr, &cancel, 32 * 1024);
    LOG_DBG("IPF", "End result=%d cancelled=%d time=%lu free=%u maxAlloc=%u", (int)result, cancel.cancelled,
            millis() - started, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    // Throttle subsequent images. A failed/cancelled image is not retried on this view.
    idleLastInput = millis();
    return;
  }
  idlePageDone = true;
}
#endif
