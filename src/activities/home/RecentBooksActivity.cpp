#include "RecentBooksActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "ReadingHistoryStore.h"
#include "ReadingStatusHelper.h"
#include "RecentBooksStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/CacheStatusIcon.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr int CACHE_STATUS_ICON_RADIUS = 7;
constexpr int HISTORY_MENU_ITEM_COUNT = 2;
constexpr int HISTORY_DELETE_HOLD_MS = 700;
constexpr StrId WEEKDAY_IDS[] = {StrId::STR_SUN, StrId::STR_MON, StrId::STR_TUE, StrId::STR_WED,
                                 StrId::STR_THU, StrId::STR_FRI, StrId::STR_SAT};
constexpr StrId HISTORY_MENU_TITLES[] = {StrId::STR_BOOK_HISTORY, StrId::STR_READING_METER};
constexpr StrId HISTORY_MENU_DESCRIPTIONS[] = {StrId::STR_BOOK_HISTORY_DESC, StrId::STR_READING_METER_DESC};
constexpr UIIcon HISTORY_MENU_ICONS[] = {UIIcon::Recent, UIIcon::Library};

Epub::CacheGenerationStatus fromCachedBookStatus(const CachedBookStatus status) {
  switch (status) {
    case CachedBookStatus::Resumable:
      return Epub::CacheGenerationStatus::Resumable;
    case CachedBookStatus::Complete:
      return Epub::CacheGenerationStatus::Complete;
    default:
      return Epub::CacheGenerationStatus::NotGenerated;
  }
}

std::string formatDuration(const uint32_t seconds) {
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  if (hours == 0) return std::to_string(minutes) + tr(STR_READING_METER_MINUTES);
  return std::to_string(hours) + tr(STR_READING_METER_HOURS) + std::to_string(minutes) + tr(STR_READING_METER_MINUTES);
}
}  // namespace

void RecentBooksActivity::loadRecentBooks() {
  recentBooks.clear();
  bookStatuses.clear();
  bookCacheStatuses.clear();
  bookDetailsLoaded.clear();
  loadBookListStatusIndex("/.crosspoint", bookListStatusIndex);
  const auto& books = READING_HISTORY.getBooks();
  recentBooks.reserve(books.size());
  bookStatuses.reserve(books.size());
  bookCacheStatuses.reserve(books.size());
  bookDetailsLoaded.reserve(books.size());

  for (const auto& book : books) {
    if (book.seconds == 0) continue;
    recentBooks.push_back({book.path, book.title, book.author, "", book.bookId});
    bookStatuses.push_back(book.finished ? ReadingStatus::Finished : ReadingStatus::Reading);
    bookCacheStatuses.push_back(Epub::CacheGenerationStatus::NotGenerated);
    bookDetailsLoaded.push_back(false);
  }
}

void RecentBooksActivity::loadVisibleBookDetails(const int pageStart, const int pageItems) {
  const int pageEnd = std::min(static_cast<int>(recentBooks.size()), pageStart + pageItems);
  for (int index = pageStart; index < pageEnd; ++index) {
    if (bookDetailsLoaded[index]) continue;
    const auto& book = recentBooks[index];
    if (FsHelpers::hasEpubExtension(book.path)) {
      ReadingStatus indexedReadingStatus = ReadingStatus::Unread;
      CachedBookStatus indexedCacheStatus = CachedBookStatus::Unknown;
      static const std::vector<std::string> unusedCacheEntries;
      if (getBookListStatusFromIndex(book.path, unusedCacheEntries, bookListStatusIndex, indexedReadingStatus,
                                     indexedCacheStatus) &&
          indexedCacheStatus != CachedBookStatus::Unknown) {
        bookCacheStatuses[index] = fromCachedBookStatus(indexedCacheStatus);
      } else {
        bookCacheStatuses[index] = Epub(book.path, "/.crosspoint").getCacheGenerationStatus();
      }
    }
    bookDetailsLoaded[index] = true;
  }
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  selectorIndex = 0;
  menuIndex = 0;
  screen = Screen::Menu;
  meterPage = MeterPage::Overview;
  booksLoaded = false;
  meterSummaryLoaded = false;
  deleteMode = DeleteMode::None;
  ignoreDeleteOpeningRelease = false;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  bookStatuses.clear();
  bookCacheStatuses.clear();
  bookDetailsLoaded.clear();
  bookListStatusIndex.clear();
  booksLoaded = false;
  meterSummaryLoaded = false;
}

void RecentBooksActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);

  if (screen == Screen::Books && deleteMode != DeleteMode::None) {
    if (ignoreDeleteOpeningRelease) {
      if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
          !mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        ignoreDeleteOpeningRelease = false;
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      bool deleted = false;
      const bool deletingAll = deleteMode == DeleteMode::All;
      if (deletingAll) {
        deleted = READING_HISTORY.clearAll();
      } else if (selectorIndex < recentBooks.size()) {
        const auto book = recentBooks[selectorIndex];
        deleted = READING_HISTORY.removeBook(book.path, book.bookId);
      }
      deleteMode = DeleteMode::None;
      if (deleted) {
        loadRecentBooks();
        meterSummaryLoaded = false;
        if (deletingAll) {
          selectorIndex = 0;
        } else if (selectorIndex >= recentBooks.size() && selectorIndex > 0) {
          --selectorIndex;
        }
      }
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      deleteMode = DeleteMode::None;
      requestUpdate();
    }
    return;
  }

  if (screen == Screen::Books && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= HISTORY_DELETE_HOLD_MS && selectorIndex < recentBooks.size()) {
    deleteMode = DeleteMode::One;
    ignoreDeleteOpeningRelease = true;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (screen == Screen::Menu) {
      screen = menuIndex == 0 ? Screen::Books : Screen::Meter;
      if (screen == Screen::Books && !booksLoaded) {
        loadRecentBooks();
        booksLoaded = true;
      } else if (screen == Screen::Meter && !meterSummaryLoaded) {
        meterSummary = READING_HISTORY.getSummary();
        meterSummaryLoaded = true;
      }
      meterPage = MeterPage::Overview;
      requestUpdate();
      return;
    }
    if (screen == Screen::Books && selectorIndex == recentBooks.size() && !recentBooks.empty()) {
      deleteMode = DeleteMode::All;
      requestUpdate();
      return;
    }
    if (screen == Screen::Books && !recentBooks.empty() && selectorIndex < recentBooks.size()) {
      LOG_DBG("RBA", "Selected recent book: %s", recentBooks[selectorIndex].path.c_str());
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
    if (screen == Screen::Meter && gpio.deviceIsX3() && !meterSummary.hasCalendarTime) {
      // X3 can start without calendar time. Let the reader recover calendar-based
      // statistics directly from the screen that explains why they are absent.
      startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) {
                               meterSummary = READING_HISTORY.getSummary();
                               meterSummaryLoaded = true;
                               meterPage = MeterPage::Overview;
                               requestUpdate();
                             });
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (screen != Screen::Menu) {
      screen = Screen::Menu;
      requestUpdate();
      return;
    }
    onGoHome();
  }

  if (screen == Screen::Meter) {
    // The X3 keeps its compact overview readable by moving the graph and book
    // ranking to a second page. X4 has sufficient room for the full dashboard.
    const auto orientation = renderer.getOrientation();
    const bool landscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                           orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
    if ((gpio.deviceIsX3() || landscape) && meterSummary.hasCalendarTime) {
      buttonNavigator.onNextRelease([this] {
        if (meterPage == MeterPage::Overview) {
          meterPage = MeterPage::Details;
          requestUpdate();
        }
      });
      buttonNavigator.onPreviousRelease([this] {
        if (meterPage == MeterPage::Details) {
          meterPage = MeterPage::Overview;
          requestUpdate();
        }
      });
    }
    return;
  }

  const int listSize = screen == Screen::Menu ? HISTORY_MENU_ITEM_COUNT : static_cast<int>(recentBooks.size()) + 1;
  size_t& selectedIndex = screen == Screen::Menu ? menuIndex : selectorIndex;

  buttonNavigator.onNextRelease([&selectedIndex, listSize, this] {
    selectedIndex = ButtonNavigator::nextIndex(static_cast<int>(selectedIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([&selectedIndex, listSize, this] {
    selectedIndex = ButtonNavigator::previousIndex(static_cast<int>(selectedIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([&selectedIndex, listSize, pageItems, this] {
    selectedIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectedIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([&selectedIndex, listSize, pageItems, this] {
    selectedIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectedIndex), listSize, pageItems);
    requestUpdate();
  });
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto layout = UiLayout::from(renderer);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool portraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int topHintGutter = portraitInverted ? metrics.buttonHintsHeight + metrics.verticalSpacing : 0;
  const int headerY = layout.content.y + metrics.topPadding + topHintGutter;
  const int bottomHints = layout.landscape ? 0 : metrics.buttonHintsHeight + metrics.verticalSpacing;
  const int centerOffset = layout.content.x + layout.content.width / 2 - renderer.getScreenWidth() / 2;
  const auto drawCentered = [&](const int fontId, const int y, const char* text,
                                const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
    renderer.drawCenteredTextOffset(fontId, y, text, true, centerOffset, style);
  };

  GUI.drawHeader(renderer, Rect{layout.content.x, headerY, layout.content.width, metrics.headerHeight},
                 tr(STR_READING_HISTORY));
  const int contentTop = headerY + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = layout.content.y + layout.content.height - bottomHints - contentTop;

  if (screen == Screen::Menu) {
    // Keep the same two-line row rhythm as the file-transfer chooser.  Lyra
    // also uses the icons here, while the Classic theme preserves its simple list style.
    GUI.drawList(
        renderer, Rect{layout.content.x, contentTop, layout.content.width, contentHeight}, HISTORY_MENU_ITEM_COUNT,
        menuIndex, [](int index) { return std::string(I18N.get(HISTORY_MENU_TITLES[index])); },
        [](int index) { return std::string(I18N.get(HISTORY_MENU_DESCRIPTIONS[index])); },
        [](int index) { return HISTORY_MENU_ICONS[index]; });
  } else if (screen == Screen::Meter) {
    const auto& summary = meterSummary;
    const bool hasCalendarTime = summary.hasCalendarTime;
    const bool isX3 = gpio.deviceIsX3();
    drawCentered(UI_12_FONT_ID, contentTop + 4, tr(STR_READING_METER));
    const bool compactMeter = isX3 || layout.landscape;
    if (compactMeter) {
      const int rowLeft = layout.content.x + metrics.contentSidePadding + 8;
      const int rowRight = layout.content.x + layout.content.width - metrics.contentSidePadding - 8;
      const auto drawOverviewRow = [&](const int y, const char* label, const std::string& value) {
        renderer.drawLine(rowLeft, y - 8, rowRight, y - 8);
        renderer.drawText(UI_10_FONT_ID, rowLeft, y, label);
        const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value.c_str());
        renderer.drawText(UI_10_FONT_ID, rowRight - valueWidth, y, value.c_str());
      };
      const auto drawTopBooks = [&](const int topY) {
        renderer.drawText(UI_10_FONT_ID, rowLeft, topY, tr(STR_READING_METER_TOP_BOOKS), true, EpdFontFamily::BOLD);
        const int maxBooks = std::min<int>(2, summary.topBookCount);
        for (int index = 0; index < maxBooks; ++index) {
          const auto& book = summary.topBooks[index];
          const std::string value = formatDuration(book.seconds);
          const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value.c_str());
          const auto title =
              renderer.truncatedText(UI_10_FONT_ID, book.title.c_str(), rowRight - rowLeft - valueWidth - 16);
          const int y = topY + 29 + index * 34;
          renderer.drawText(UI_10_FONT_ID, rowLeft, y, title.c_str());
          renderer.drawText(UI_10_FONT_ID, rowRight - valueWidth, y, value.c_str());
        }
      };

      if (meterPage == MeterPage::Overview || !hasCalendarTime) {
        const std::string primaryLabel = hasCalendarTime ? tr(STR_READING_METER_WEEK) : tr(STR_READING_METER_TOTAL);
        const uint32_t primarySeconds = hasCalendarTime ? summary.weekSeconds : summary.totalSeconds;
        drawCentered(UI_10_FONT_ID, contentTop + 43, primaryLabel.c_str());
        drawCentered(UI_12_FONT_ID, contentTop + 68, formatDuration(primarySeconds).c_str(), EpdFontFamily::BOLD);
        int rowY = contentTop + 125;
        if (hasCalendarTime) {
          drawOverviewRow(rowY, tr(STR_READING_METER_TODAY), formatDuration(summary.todaySeconds));
          rowY += 38;
          drawOverviewRow(rowY, tr(STR_READING_METER_MONTH), formatDuration(summary.monthSeconds));
          rowY += 38;
        }
        const std::string books = std::to_string(summary.bookCount) + tr(STR_READING_METER_BOOKS_UNIT);
        const std::string finished = std::to_string(summary.finishedBookCount) + tr(STR_READING_METER_BOOKS_UNIT);
        drawOverviewRow(rowY, tr(STR_READING_METER_BOOKS), books);
        drawOverviewRow(rowY + 38, tr(STR_READING_METER_FINISHED), finished);
        renderer.drawLine(rowLeft, rowY + 68, rowRight, rowY + 68);
        if (!hasCalendarTime) {
          drawCentered(UI_10_FONT_ID, rowY + 104, tr(STR_READING_METER_TIME_UNAVAILABLE));
          drawCentered(UI_10_FONT_ID, rowY + 128, tr(STR_READING_METER_TIME_SYNC_HINT));
        }
      } else {
        const int graphLeft = layout.content.x + metrics.contentSidePadding + 18;
        const int graphWidth = layout.content.width - (metrics.contentSidePadding + 18) * 2;
        // Leave the same clear separation below the 12pt meter title as the
        // overview page.  The old position put the graph label into the title
        // glyph bounds on X3.
        const int graphTop = contentTop + 74;
        const int graphHeight = std::max(72, std::min(110, contentHeight / 3));
        const int baselineY = graphTop + graphHeight;
        uint32_t maximum = 0;
        for (const auto seconds : summary.recentDaySeconds) maximum = std::max(maximum, seconds);
        renderer.drawText(UI_10_FONT_ID, graphLeft, graphTop - 24, tr(STR_READING_METER_LAST_7_DAYS));
        renderer.drawLine(graphLeft, baselineY, graphLeft + graphWidth, baselineY);
        const int columnWidth = graphWidth / 7;
        const int barWidth = std::max(5, columnWidth - 14);
        for (int index = 0; index < 7; ++index) {
          const int centerX = graphLeft + index * columnWidth + columnWidth / 2;
          if (maximum > 0 && summary.recentDaySeconds[index] > 0) {
            const int barHeight =
                std::max(3, static_cast<int>(summary.recentDaySeconds[index] * graphHeight / maximum));
            renderer.fillRect(centerX - barWidth / 2, baselineY - barHeight, barWidth, barHeight);
          }
          const char* label = I18N.get(WEEKDAY_IDS[summary.recentDayWeekdays[index]]);
          const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
          renderer.drawText(UI_10_FONT_ID, centerX - textWidth / 2, baselineY + 10, label);
        }
        drawTopBooks(baselineY + 37);
      }
    } else {
      const auto drawBookSummary = [this, &summary, &layout, &metrics, &drawCentered](const int topY) {
        const std::string bookCount = std::string(tr(STR_READING_METER_BOOKS)) + ": " +
                                      std::to_string(summary.bookCount) + tr(STR_READING_METER_BOOKS_UNIT);
        drawCentered(UI_10_FONT_ID, topY, bookCount.c_str());
        const std::string finishedCount = std::string(tr(STR_READING_METER_FINISHED)) + ": " +
                                          std::to_string(summary.finishedBookCount) + tr(STR_READING_METER_BOOKS_UNIT);
        drawCentered(UI_10_FONT_ID, topY + 22, finishedCount.c_str());
        const int left = layout.content.x + metrics.contentSidePadding;
        const int right = layout.content.x + layout.content.width - metrics.contentSidePadding;
        renderer.drawText(UI_10_FONT_ID, left, topY + 64, tr(STR_READING_METER_TOP_BOOKS), true, EpdFontFamily::BOLD);
        for (uint8_t index = 0; index < summary.topBookCount; ++index) {
          const auto& book = summary.topBooks[index];
          const std::string value = formatDuration(book.seconds);
          const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value.c_str());
          const int titleWidth = layout.content.width - metrics.contentSidePadding * 2 - valueWidth - 18;
          const auto title = renderer.truncatedText(UI_10_FONT_ID, book.title.c_str(), titleWidth);
          const int y = topY + 99 + index * 38;
          renderer.drawText(UI_10_FONT_ID, left, y, title.c_str());
          renderer.drawText(UI_10_FONT_ID, right - valueWidth, y, value.c_str());
        }
      };
      if (hasCalendarTime) {
        const std::string today =
            std::string(tr(STR_READING_METER_TODAY)) + ": " + formatDuration(summary.todaySeconds);
        const std::string week = std::string(tr(STR_READING_METER_WEEK)) + ": " + formatDuration(summary.weekSeconds);
        const std::string month =
            std::string(tr(STR_READING_METER_MONTH)) + ": " + formatDuration(summary.monthSeconds);
        drawCentered(UI_10_FONT_ID, contentTop + 43, today.c_str());
        drawCentered(UI_10_FONT_ID, contentTop + 70, week.c_str());
        drawCentered(UI_10_FONT_ID, contentTop + 97, month.c_str());

        uint32_t maximum = 0;
        for (const auto seconds : summary.recentDaySeconds) maximum = std::max(maximum, seconds);
        if (maximum == 0) {
          // A large empty graph is less useful than the books that have been read
          // before this week. Keep the time summary, then bring those books closer.
          drawCentered(UI_10_FONT_ID, contentTop + 150, tr(STR_READING_METER_NO_RECENT_ACTIVITY));
          drawBookSummary(contentTop + 185);
        } else {
          const int graphLeft = layout.content.x + metrics.contentSidePadding + 18;
          const int graphWidth = layout.content.width - (metrics.contentSidePadding + 18) * 2;
          const int graphTop = contentTop + 145;
          const int graphHeight = std::max(80, std::min(170, contentHeight - 215));
          const int baselineY = graphTop + graphHeight;
          renderer.drawText(UI_10_FONT_ID, graphLeft, graphTop - 24, tr(STR_READING_METER_LAST_7_DAYS));
          renderer.drawLine(graphLeft, baselineY, graphLeft + graphWidth, baselineY);
          const int columnWidth = graphWidth / 7;
          const int barWidth = std::max(6, columnWidth - 16);
          for (int index = 0; index < 7; ++index) {
            const int centerX = graphLeft + index * columnWidth + columnWidth / 2;
            if (maximum > 0 && summary.recentDaySeconds[index] > 0) {
              const int barHeight =
                  std::max(3, static_cast<int>(summary.recentDaySeconds[index] * graphHeight / maximum));
              renderer.fillRect(centerX - barWidth / 2, baselineY - barHeight, barWidth, barHeight);
            }
            const char* label = I18N.get(WEEKDAY_IDS[summary.recentDayWeekdays[index]]);
            const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
            renderer.drawText(UI_10_FONT_ID, centerX - textWidth / 2, baselineY + 12, label);
          }
          drawBookSummary(baselineY + 45);
        }
      } else {
        // X4 normally has no clock after a full power-off. Show persistent,
        // useful statistics instead of an empty daily graph.
        drawCentered(UI_10_FONT_ID, contentTop + 34, tr(STR_READING_METER_TOTAL));
        drawCentered(UI_12_FONT_ID, contentTop + 58, formatDuration(summary.totalSeconds).c_str(), EpdFontFamily::BOLD);
        drawBookSummary(contentTop + 96);
      }
      const std::string total = std::string(tr(STR_READING_METER_TOTAL)) + ": " + formatDuration(summary.totalSeconds);
      if (hasCalendarTime) drawCentered(UI_10_FONT_ID, contentTop + contentHeight - 25, total.c_str());
    }
  } else if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, layout.content.x + metrics.contentSidePadding, contentTop + 20,
                      tr(STR_NO_BOOK_HISTORY));
  } else {
    const int rowHeight = metrics.listWithSubtitleRowHeight;
    const int deleteHintHeight = renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
    const int listHeight = deleteMode == DeleteMode::None ? contentHeight - deleteHintHeight : contentHeight;
    const int pageItems = std::max(1, listHeight / rowHeight);
    const int pageStart = (selectorIndex / pageItems) * pageItems;
    loadVisibleBookDetails(pageStart, pageItems);

    if (deleteMode != DeleteMode::None) {
      const bool deleteAll = deleteMode == DeleteMode::All;
      drawCentered(UI_10_FONT_ID, contentTop + contentHeight / 2 - 25,
                   deleteAll ? tr(STR_CONFIRM_CLEAR_READING_HISTORY) : tr(STR_CONFIRM_DELETE_HISTORY_BOOK),
                   EpdFontFamily::BOLD);
      if (!deleteAll && selectorIndex < recentBooks.size()) {
        const auto title = renderer.truncatedText(UI_10_FONT_ID, recentBooks[selectorIndex].title.c_str(),
                                                  layout.content.width - metrics.contentSidePadding * 2);
        drawCentered(UI_10_FONT_ID, contentTop + contentHeight / 2 + 10, title.c_str());
      }
    } else {
      const int itemCount = static_cast<int>(recentBooks.size()) + 1;
      GUI.drawList(
          renderer, Rect{layout.content.x, contentTop, layout.content.width, listHeight}, itemCount, selectorIndex,
          [this](int index) {
            return index == static_cast<int>(recentBooks.size()) ? std::string(tr(STR_CLEAR_ALL_READING_HISTORY))
                                                                 : recentBooks[index].title;
          },
          [this](int index) {
            return index == static_cast<int>(recentBooks.size()) ? std::string() : recentBooks[index].author;
          },
          [this](int index) {
            return index == static_cast<int>(recentBooks.size())
                       ? UIIcon::Settings
                       : UITheme::getFileIcon(recentBooks[index].path, bookStatuses[index]);
          });
      drawCentered(UI_10_FONT_ID, contentTop + listHeight + metrics.verticalSpacing / 2,
                   tr(STR_HOLD_SELECT_TO_DELETE_HISTORY));
    }

    const int iconCenterX =
        layout.content.x + layout.content.width - metrics.contentSidePadding - CACHE_STATUS_ICON_RADIUS - 10;
    for (int index = pageStart; index < static_cast<int>(recentBooks.size()) && index < pageStart + pageItems;
         ++index) {
      if (deleteMode != DeleteMode::None) break;
      if (!FsHelpers::hasEpubExtension(recentBooks[index].path)) continue;
      const int iconCenterY = contentTop + (index - pageStart) * rowHeight + rowHeight / 2;
      const bool ink = UITheme::getInstance().getTheme().showsFileIcons() || index != selectorIndex;
      CacheStatusIcon::draw(renderer, bookCacheStatuses[index], CACHE_STATUS_ICON_RADIUS, iconCenterX, iconCenterY,
                            ink);
    }
  }

  // Help text
  const char* backLabel =
      deleteMode != DeleteMode::None ? tr(STR_CANCEL) : (screen == Screen::Menu ? tr(STR_HOME) : tr(STR_BACK));
  const bool x3MeterTimeRecovery = screen == Screen::Meter && gpio.deviceIsX3() && !meterSummary.hasCalendarTime;
  const char* confirmLabel =
      deleteMode != DeleteMode::None
          ? tr(STR_DELETE)
          : (screen == Screen::Menu ? tr(STR_SELECT)
                                    : (x3MeterTimeRecovery ? tr(STR_READING_METER_SYNC_TIME)
                                                           : (screen == Screen::Meter ? "" : tr(STR_OPEN))));
  const bool compactMeterPaging =
      screen == Screen::Meter && (gpio.deviceIsX3() || layout.landscape) && meterSummary.hasCalendarTime;
  const char* previousLabel = compactMeterPaging && meterPage == MeterPage::Details
                                  ? tr(STR_PREVIOUS)
                                  : (screen == Screen::Meter ? "" : tr(STR_DIR_UP));
  const char* nextLabel = compactMeterPaging && meterPage == MeterPage::Overview
                              ? tr(STR_NEXT)
                              : (screen == Screen::Meter ? "" : tr(STR_DIR_DOWN));
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, previousLabel, nextLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
