#include "Lyra3CoversTheme.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/CacheStatusIcon.h"
#include "components/UITheme.h"
#include "components/icons/book_finished24.h"
#include "components/icons/book_reading24.h"
#include "components/icons/bookmark24.h"
#include "components/icons/cover.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;

void drawHomeProgressBar(const GfxRenderer& renderer, const Rect rect, int percent) {
  if (rect.width <= 4 || rect.height <= 4) return;
  percent = std::clamp(percent, 0, 100);
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
  const int innerWidth = rect.width - 4;
  const int fillWidth = (innerWidth * percent + 99) / 100;
  if (fillWidth > 0) renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
}
}  // namespace

int Lyra3CoversTheme::getHomeRecentBooksCount(const GfxRenderer& renderer) const {
  const auto orientation = renderer.getOrientation();
  const bool landscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                         orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  return landscape ? 2 : Lyra3CoversMetrics::values.homeRecentBooksCount;
}

int Lyra3CoversTheme::getHomePortraitMenuTop(const GfxRenderer& renderer) const {
  constexpr int menuItemCount = 5;
  const int menuHeight =
      menuItemCount * (Lyra3CoversMetrics::values.menuRowHeight + Lyra3CoversMetrics::values.menuSpacing);
  return renderer.getScreenHeight() - Lyra3CoversMetrics::values.buttonHintsHeight - menuHeight;
}

void Lyra3CoversTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                           const std::vector<ReadingProgress>& bookProgress, const int selectorIndex,
                                           bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                                           std::function<bool()> storeCoverBuffer) const {
  const int coverCount = getHomeRecentBooksCount(renderer);
  const int coverHeight = getHomeCoverHeight(renderer);
  const int tileWidth = (rect.width - 2 * Lyra3CoversMetrics::values.contentSidePadding) / coverCount;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    if (!coverRendered) {
      for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), coverCount); i++) {
        std::string coverPath = recentBooks[i].coverBmpPath;
        bool hasCover = true;
        int tileX = rect.x + Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;
        if (coverPath.empty()) {
          hasCover = false;
        } else {
          const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, coverHeight);

          // First time: load cover from SD and render
          FsFile file;
          if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              const float bitmapHeight = static_cast<float>(bitmap.getHeight());
              const float bitmapWidth = static_cast<float>(bitmap.getWidth());
              const float ratio = bitmapWidth / bitmapHeight;
              const float tileRatio =
                  static_cast<float>(tileWidth - 2 * hPaddingInSelection) / static_cast<float>(coverHeight);
              float cropX = 1.0f - (tileRatio / ratio);

              renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                  tileWidth - 2 * hPaddingInSelection, coverHeight, cropX);
            } else {
              hasCover = false;
            }
            file.close();
          }
        }
        // Draw either way
        renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, tileWidth - 2 * hPaddingInSelection,
                          coverHeight, true);

        if (!hasCover) {
          // Render empty cover
          renderer.fillRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection + (coverHeight / 3),
                            tileWidth - 2 * hPaddingInSelection, 2 * coverHeight / 3, true);
          renderer.drawIcon(CoverIcon, tileX + hPaddingInSelection + 24, tileY + hPaddingInSelection + 24, 32, 32);
        }
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), coverCount); i++) {
      bool bookSelected = (selectorIndex == i);

      int tileX = rect.x + Lyra3CoversMetrics::values.contentSidePadding + tileWidth * i;

      const int maxLineWidth = tileWidth - 2 * hPaddingInSelection;

      auto titleLines = renderer.wrappedText(SMALL_FONT_ID, recentBooks[i].title.c_str(), maxLineWidth, 2);

      constexpr int statusIconSize = 24;
      constexpr int statusIconTopMargin = 4;
      const bool hasProgressData = i < static_cast<int>(bookProgress.size());
      const ReadingStatus readingStatus = hasProgressData ? bookProgress[i].status : ReadingStatus::Unread;
      const bool hasReadingStatusIcon =
          readingStatus == ReadingStatus::Reading || readingStatus == ReadingStatus::Finished;
      const bool hasBookmarkIcon = hasProgressData && bookProgress[i].hasBookmarks;
      const bool hasCacheStatusIcon = FsHelpers::hasEpubExtension(recentBooks[i].path);
      const Epub::CacheGenerationStatus cacheStatus =
          hasCacheStatusIcon ? Epub(recentBooks[i].path, "/.crosspoint").getCacheGenerationStatus()
                             : Epub::CacheGenerationStatus::NotGenerated;
      const bool hasStatusIcons = hasBookmarkIcon || hasReadingStatusIcon || hasCacheStatusIcon;
      const bool hasProgressBar = hasProgressData && bookProgress[i].hasPercent();

      const int titleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
      // Reserve two title lines and one status row for every card. This keeps
      // icons and progress bars aligned when neighbouring titles have
      // different lengths or one book has no status icon.
      constexpr int reservedTitleLines = 2;
      const int titleBlockHeight = reservedTitleLines * titleLineHeight;
      const int statusBlockHeight = statusIconSize + statusIconTopMargin;
      constexpr int progressTopMargin = 6;
      constexpr int progressBarHeight = 10;
      const int progressBlockHeight = progressTopMargin + progressBarHeight;
      // Add a little padding below the text inside the selection box just like the top padding (5 + hPaddingSelection)
      const int dynamicTitleBoxHeight =
          titleBlockHeight + statusBlockHeight + progressBlockHeight + hPaddingInSelection + 5;

      if (bookSelected) {
        // Draw selection box
        renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                                 Color::LightGray);
        renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection, coverHeight, Color::LightGray);
        renderer.fillRectDither(tileX + tileWidth - hPaddingInSelection, tileY + hPaddingInSelection,
                                hPaddingInSelection, coverHeight, Color::LightGray);
        renderer.fillRoundedRect(tileX, tileY + coverHeight + hPaddingInSelection, tileWidth, dynamicTitleBoxHeight,
                                 cornerRadius, false, false, true, true, Color::LightGray);
      }

      int currentY = tileY + coverHeight + hPaddingInSelection + 5;
      for (const auto& line : titleLines) {
        renderer.drawText(SMALL_FONT_ID, tileX + hPaddingInSelection, currentY, line.c_str(), true);
        currentY += titleLineHeight;
      }
      currentY = tileY + coverHeight + hPaddingInSelection + 5 + titleBlockHeight;
      if (hasStatusIcons) {
        const int iconY = currentY + statusIconTopMargin;
        int iconX = tileX + hPaddingInSelection;
        if (hasReadingStatusIcon) {
          const uint8_t* iconBitmap = readingStatus == ReadingStatus::Finished ? BookFinished24Icon : BookReading24Icon;
          renderer.drawIcon(iconBitmap, iconX, iconY, statusIconSize, statusIconSize);
          iconX += statusIconSize + 6;
        }
        if (hasBookmarkIcon) {
          renderer.drawIcon(Bookmark24Icon, iconX, iconY, statusIconSize, statusIconSize);
          iconX += statusIconSize + 6;
        }
        if (hasCacheStatusIcon) {
          constexpr int cacheStatusIconRadius = 7;
          CacheStatusIcon::draw(renderer, cacheStatus, cacheStatusIconRadius, iconX + cacheStatusIconRadius,
                                iconY + statusIconSize / 2);
        }
      }
      currentY += statusIconTopMargin + statusIconSize;
      if (hasProgressBar) {
        currentY += progressTopMargin;
        const int displayPercent = readingStatus == ReadingStatus::Finished ? 100 : bookProgress[i].percent;
        char percentText[8];
        snprintf(percentText, sizeof(percentText), "%u%%", static_cast<unsigned>(displayPercent));
        const bool showPercent = readingStatus != ReadingStatus::Finished;
        const int percentWidth = showPercent ? renderer.getTextWidth(SMALL_FONT_ID, percentText) : 0;
        constexpr int percentGap = 6;
        const int barX = tileX + hPaddingInSelection;
        const int barWidth = maxLineWidth - (showPercent ? percentWidth + percentGap : 0);
        drawHomeProgressBar(renderer, Rect{barX, currentY, barWidth, progressBarHeight}, displayPercent);
        if (showPercent) {
          const int textY = currentY + (progressBarHeight - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
          renderer.drawText(SMALL_FONT_ID, barX + barWidth + percentGap, textY, percentText, true);
        }
      }
    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}
