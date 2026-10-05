#include "SnapshotPositionProjector.h"

#include <Epub/Section.h>
#include <FontCacheManager.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

#include "CrossPointSettings.h"
#include "ReadingStatusHelper.h"
#include "SdCardFontGlobals.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookDataPath.h"
namespace yomuka::sync {
bool SnapshotPositionProjector::project(const Progress& source, const BookReaderSettings::Override* incoming,
                                        Progress& output, void* context) {
  auto& self = *static_cast<SnapshotPositionProjector*>(context);
  const auto savedSettings = BookReaderSettings::captureAll(SETTINGS);
  const auto savedOrientation = self.renderer.getOrientation();
  bool ok = false;
  {
    std::unique_ptr<RenderLock> lock;
    if (!self.renderLocked) lock = std::make_unique<RenderLock>();
    HalPowerManager::Lock power;
    if (self.cancel && self.cancel()) return false;
    // Global fallback first; if settings are unselected use current override.
    BookReaderSettings::Override local;
    if (incoming)
      BookReaderSettings::apply(*incoming, SETTINGS);
    else {
      uint32_t date = 0;
      const auto status = BookReaderSettings::readForSync(self.book.id, local, date);
      if (status != ReadStatus::Present && status != ReadStatus::Absent) {
        BookReaderSettings::apply(savedSettings, SETTINGS);
        return false;
      }
      BookReaderSettings::apply(local, SETTINGS);
    }
    auto restore = [&] {
      sdFontSystem.releaseLoadedFamily(self.renderer);
      BookReaderSettings::apply(savedSettings, SETTINGS);
      self.renderer.setOrientation(savedOrientation);
    };
    bool vertical = SETTINGS.writingMode == CrossPointSettings::WM_VERTICAL ||
                    (SETTINGS.writingMode == CrossPointSettings::WM_AUTO && self.epub->isPageProgressionRtl() &&
                     (self.epub->getLanguage() == "ja" || self.epub->getLanguage() == "jpn" ||
                      self.epub->getLanguage() == "zh" || self.epub->getLanguage() == "zho"));
    switch (SETTINGS.orientation) {
      case CrossPointSettings::LANDSCAPE_CW:
        self.renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
        break;
      case CrossPointSettings::LANDSCAPE_CCW:
        self.renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
        break;
      case CrossPointSettings::INVERTED:
        self.renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
        break;
      default:
        self.renderer.setOrientation(GfxRenderer::Orientation::Portrait);
        break;
    }
    ensureSdFontLoaded(vertical);
    configureRubyFont(vertical);
    configureSmallFont(vertical);
    const auto& ds = SETTINGS.getDirectionSettings(vertical);
    if (*ds.sdFontFamilyName && !sdFontSystem.resolveFontId(ds.sdFontFamilyName, ds.fontSize)) {
      restore();
      return false;
    }
    ResolvedChapterLayout layout;
    if (source.spineIndex < self.book.spines) {
      int top, right, bottom, left;
      self.renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
      const int bar = UITheme::getInstance().getStatusBarHeight();
      const int width = self.renderer.getScreenWidth() - left - right - ds.screenMargin * 2;
      const int height = self.renderer.getScreenHeight() - top - bottom - ds.screenMargin -
                         std::max<int>(ds.screenMargin, bar > 0 ? bar + 8 : 0);
      if (width <= 0 || height <= 0) {
        restore();
        return false;
      }
      self.renderer.setVerticalCharSpacing(SETTINGS.getVerticalCharSpacingPercent());
      if (auto* cache = self.renderer.getFontCacheManager()) {
        cache->clearCache();
        cache->freeKernLigatureData();
      }
      Section section(self.epub, source.spineIndex, self.renderer);
      const float compression = SETTINGS.getReaderLineCompression(vertical);
      bool ready = section.loadSectionFile(SETTINGS.getReaderFontId(vertical), SETTINGS.getTableFontId(vertical),
                                           compression, ds.extraParagraphSpacing, ds.paragraphAlignment, width, height,
                                           ds.hyphenationEnabled, ds.firstLineIndent, SETTINGS.embeddedStyle,
                                           SETTINGS.imageRendering, vertical, ds.charSpacing, ds.tateChuYokoMaxDigits);
      if (!ready) {
        const int headings[6] = {
            SETTINGS.getHeadingFontId(1, vertical), SETTINGS.getHeadingFontId(2, vertical), 0, 0, 0, 0};
        const int body[4] = {SETTINGS.getReaderFontIdForSize(vertical, 0), SETTINGS.getReaderFontIdForSize(vertical, 1),
                             SETTINGS.getReaderFontIdForSize(vertical, 2),
                             SETTINGS.getReaderFontIdForSize(vertical, 3)};
        ready = section.createSectionFile(SETTINGS.getReaderFontId(vertical), compression, ds.extraParagraphSpacing,
                                          ds.paragraphAlignment, width, height, ds.hyphenationEnabled,
                                          ds.firstLineIndent, SETTINGS.embeddedStyle, SETTINGS.imageRendering, vertical,
                                          ds.charSpacing, ds.tateChuYokoMaxDigits, nullptr, headings,
                                          SETTINGS.getTableFontId(vertical), body, nullptr, nullptr, self.cancel, true);
      }
      layout = {ready, section.pageCount};
    }
    ProjectedProgress projected;
    if (projectProgressForBook(source, self.book.spines, layout, projected) == ProgressError::None) {
      Progress target = source;
      target.chapterPage = projected.chapterPage;
      target.chapterPageCount = projected.chapterPageCount;
      if (!target.percent) {
        const float ratio =
            projected.chapterPageCount ? float(projected.chapterPage + 1) / projected.chapterPageCount : 0;
        target.percent =
            projected.terminal
                ? 100
                : std::clamp<int>(std::lround(self.epub->calculateProgress(projected.spineIndex, ratio) * 100), 0, 100);
      }
      if (!target.finished) target.finished = *target.percent >= 95;
      output = target;
      ok = true;
    }
    restore();
  }
  return ok;
}
}  // namespace yomuka::sync
