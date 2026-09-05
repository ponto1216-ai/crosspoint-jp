#include "RecentBooksActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <FsHelpers.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "ReadingStatusHelper.h"
#include "ReadingHistoryStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/CacheStatusIcon.h"
#include "fontIds.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr int CACHE_STATUS_ICON_RADIUS = 7;
constexpr int HISTORY_MENU_ITEM_COUNT = 2;
constexpr StrId WEEKDAY_IDS[] = {StrId::STR_SUN, StrId::STR_MON, StrId::STR_TUE, StrId::STR_WED, StrId::STR_THU,
                                 StrId::STR_FRI, StrId::STR_SAT};
constexpr StrId HISTORY_MENU_TITLES[] = {StrId::STR_BOOK_HISTORY, StrId::STR_READING_METER};
constexpr StrId HISTORY_MENU_DESCRIPTIONS[] = {StrId::STR_BOOK_HISTORY_DESC, StrId::STR_READING_METER_DESC};
constexpr UIIcon HISTORY_MENU_ICONS[] = {UIIcon::Recent, UIIcon::Library};

std::string formatDuration(const uint32_t seconds) {
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  if (hours == 0) return std::to_string(minutes) + tr(STR_READING_METER_MINUTES);
  return std::to_string(hours) + tr(STR_READING_METER_HOURS) + std::to_string(minutes) +
         tr(STR_READING_METER_MINUTES);
}
}  // namespace

void RecentBooksActivity::loadRecentBooks() {
  recentBooks.clear();
  bookStatuses.clear();
  bookCacheStatuses.clear();
  const auto& books = READING_HISTORY.getBooks();
  recentBooks.reserve(books.size());
  bookStatuses.reserve(books.size());
  bookCacheStatuses.reserve(books.size());

  for (const auto& book : books) {
    if (book.seconds == 0 || !Storage.exists(book.path.c_str())) continue;
    recentBooks.push_back({book.path, book.title, book.author, "", book.bookId});
    bookStatuses.push_back(getReadingStatus(book.path, "/.crosspoint", book.bookId));
    bookCacheStatuses.push_back(FsHelpers::hasEpubExtension(book.path)
                                    ? Epub(book.path, "/.crosspoint").getCacheGenerationStatus()
                                    : Epub::CacheGenerationStatus::NotGenerated);
  }
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Load data
  loadRecentBooks();

  selectorIndex = 0;
  menuIndex = 0;
  screen = Screen::Menu;
  meterPage = MeterPage::Overview;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  bookStatuses.clear();
  bookCacheStatuses.clear();
}

void RecentBooksActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (screen == Screen::Menu) {
      screen = menuIndex == 0 ? Screen::Books : Screen::Meter;
      meterPage = MeterPage::Overview;
      requestUpdate();
      return;
    }
    if (screen == Screen::Books && !recentBooks.empty() && selectorIndex < static_cast<int>(recentBooks.size())) {
      LOG_DBG("RBA", "Selected recent book: %s", recentBooks[selectorIndex].path.c_str());
      onSelectBook(recentBooks[selectorIndex].path);
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
    if (gpio.deviceIsX3() && READING_HISTORY.getSummary().hasCalendarTime) {
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

  const int listSize = screen == Screen::Menu ? HISTORY_MENU_ITEM_COUNT : static_cast<int>(recentBooks.size());
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

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_HISTORY));
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (screen == Screen::Menu) {
    // Keep the same two-line row rhythm as the file-transfer chooser.  Lyra
    // also uses the icons here, while the Classic theme preserves its simple list style.
    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, HISTORY_MENU_ITEM_COUNT, menuIndex,
                 [](int index) { return std::string(I18N.get(HISTORY_MENU_TITLES[index])); },
                 [](int index) { return std::string(I18N.get(HISTORY_MENU_DESCRIPTIONS[index])); },
                 [](int index) { return HISTORY_MENU_ICONS[index]; });
  } else if (screen == Screen::Meter) {
    const auto summary = READING_HISTORY.getSummary();
    const bool isX3 = gpio.deviceIsX3();
    renderer.drawCenteredText(UI_12_FONT_ID, contentTop + 4, tr(STR_READING_METER));
    if (isX3) {
      const int rowLeft = metrics.contentSidePadding + 8;
      const int rowRight = pageWidth - metrics.contentSidePadding - 8;
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
          const auto title = renderer.truncatedText(UI_10_FONT_ID, book.title.c_str(), rowRight - rowLeft - valueWidth - 16);
          const int y = topY + 29 + index * 34;
          renderer.drawText(UI_10_FONT_ID, rowLeft, y, title.c_str());
          renderer.drawText(UI_10_FONT_ID, rowRight - valueWidth, y, value.c_str());
        }
      };

      if (meterPage == MeterPage::Overview || !summary.hasCalendarTime) {
        const std::string primaryLabel = summary.hasCalendarTime ? tr(STR_READING_METER_WEEK) : tr(STR_READING_METER_TOTAL);
        const uint32_t primarySeconds = summary.hasCalendarTime ? summary.weekSeconds : summary.totalSeconds;
        renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 43, primaryLabel.c_str());
        renderer.drawCenteredText(UI_12_FONT_ID, contentTop + 68, formatDuration(primarySeconds).c_str(), true,
                                  EpdFontFamily::BOLD);
        int rowY = contentTop + 125;
        if (summary.hasCalendarTime) {
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
      } else {
        const int graphLeft = metrics.contentSidePadding + 18;
        const int graphWidth = pageWidth - graphLeft * 2;
        const int graphTop = contentTop + 48;
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
            const int barHeight = std::max(3, static_cast<int>(summary.recentDaySeconds[index] * graphHeight / maximum));
            renderer.fillRect(centerX - barWidth / 2, baselineY - barHeight, barWidth, barHeight);
          }
          const char* label = I18N.get(WEEKDAY_IDS[summary.recentDayWeekdays[index]]);
          const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
          renderer.drawText(UI_10_FONT_ID, centerX - textWidth / 2, baselineY + 10, label);
        }
        drawTopBooks(baselineY + 37);
      }
    } else {
    const auto drawBookSummary = [this, &summary, pageWidth, contentTop, &metrics](const int topY) {
      const std::string bookCount = std::string(tr(STR_READING_METER_BOOKS)) + ": " +
                                    std::to_string(summary.bookCount) + tr(STR_READING_METER_BOOKS_UNIT);
      renderer.drawCenteredText(UI_10_FONT_ID, topY, bookCount.c_str());
      const std::string finishedCount = std::string(tr(STR_READING_METER_FINISHED)) + ": " +
                                        std::to_string(summary.finishedBookCount) + tr(STR_READING_METER_BOOKS_UNIT);
      renderer.drawCenteredText(UI_10_FONT_ID, topY + 22, finishedCount.c_str());
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, topY + 64, tr(STR_READING_METER_TOP_BOOKS), true,
                        EpdFontFamily::BOLD);
      for (uint8_t index = 0; index < summary.topBookCount; ++index) {
        const auto& book = summary.topBooks[index];
        const std::string value = formatDuration(book.seconds);
        const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value.c_str());
        const int titleWidth = pageWidth - metrics.contentSidePadding * 2 - valueWidth - 18;
        const auto title = renderer.truncatedText(UI_10_FONT_ID, book.title.c_str(), titleWidth);
        const int y = topY + 99 + index * 38;
        renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, title.c_str());
        renderer.drawText(UI_10_FONT_ID, pageWidth - metrics.contentSidePadding - valueWidth, y, value.c_str());
      }
    };
    if (summary.hasCalendarTime) {
      const std::string today = std::string(tr(STR_READING_METER_TODAY)) + ": " + formatDuration(summary.todaySeconds);
      const std::string week = std::string(tr(STR_READING_METER_WEEK)) + ": " + formatDuration(summary.weekSeconds);
      const std::string month = std::string(tr(STR_READING_METER_MONTH)) + ": " + formatDuration(summary.monthSeconds);
      renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 43, today.c_str());
      renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 70, week.c_str());
      renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 97, month.c_str());

      uint32_t maximum = 0;
      for (const auto seconds : summary.recentDaySeconds) maximum = std::max(maximum, seconds);
      if (maximum == 0) {
        // A large empty graph is less useful than the books that have been read
        // before this week. Keep the time summary, then bring those books closer.
        renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 150, tr(STR_READING_METER_NO_RECENT_ACTIVITY));
        drawBookSummary(contentTop + 185);
      } else {
      const int graphLeft = metrics.contentSidePadding + 18;
      const int graphWidth = pageWidth - graphLeft * 2;
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
          const int barHeight = std::max(3, static_cast<int>(summary.recentDaySeconds[index] * graphHeight / maximum));
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
      renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 34, tr(STR_READING_METER_TOTAL));
      renderer.drawCenteredText(UI_12_FONT_ID, contentTop + 58, formatDuration(summary.totalSeconds).c_str(), true,
                                EpdFontFamily::BOLD);
      drawBookSummary(contentTop + 96);
    }
    const std::string total = std::string(tr(STR_READING_METER_TOTAL)) + ": " + formatDuration(summary.totalSeconds);
    if (summary.hasCalendarTime) renderer.drawCenteredText(UI_10_FONT_ID, contentTop + contentHeight - 25, total.c_str());
    }
  } else if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_BOOK_HISTORY));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, recentBooks.size(), selectorIndex,
        [this](int index) { return recentBooks[index].title; }, [this](int index) { return recentBooks[index].author; },
        [this](int index) { return UITheme::getFileIcon(recentBooks[index].path, bookStatuses[index]); });

    const int rowHeight = metrics.listWithSubtitleRowHeight;
    const int pageItems = std::max(1, contentHeight / rowHeight);
    const int pageStart = (selectorIndex / pageItems) * pageItems;
    const int iconCenterX = pageWidth - metrics.contentSidePadding - CACHE_STATUS_ICON_RADIUS - 10;
    for (int index = pageStart; index < static_cast<int>(recentBooks.size()) && index < pageStart + pageItems; ++index) {
      if (!FsHelpers::hasEpubExtension(recentBooks[index].path)) continue;
      const int iconCenterY = contentTop + (index - pageStart) * rowHeight + rowHeight / 2;
      const bool ink = UITheme::getInstance().getTheme().showsFileIcons() || index != selectorIndex;
      CacheStatusIcon::draw(renderer, bookCacheStatuses[index], CACHE_STATUS_ICON_RADIUS, iconCenterX, iconCenterY, ink);
    }
  }

  // Help text
  const char* backLabel = screen == Screen::Menu ? tr(STR_HOME) : tr(STR_BACK);
  const char* confirmLabel = screen == Screen::Menu ? tr(STR_SELECT) : (screen == Screen::Meter ? "" : tr(STR_OPEN));
  const bool x3MeterPaging = screen == Screen::Meter && gpio.deviceIsX3() &&
                              READING_HISTORY.getSummary().hasCalendarTime;
  const char* previousLabel = x3MeterPaging && meterPage == MeterPage::Details
                                  ? tr(STR_PREVIOUS)
                                  : (screen == Screen::Meter ? "" : tr(STR_DIR_UP));
  const char* nextLabel = x3MeterPaging && meterPage == MeterPage::Overview
                              ? tr(STR_NEXT)
                              : (screen == Screen::Meter ? "" : tr(STR_DIR_DOWN));
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, previousLabel, nextLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
