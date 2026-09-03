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
constexpr char CACHE_STATUS_VALUE_SPACER[] = "    ";
constexpr int HISTORY_MENU_ITEM_COUNT = 2;
constexpr StrId WEEKDAY_IDS[] = {StrId::STR_SUN, StrId::STR_MON, StrId::STR_TUE, StrId::STR_WED, StrId::STR_THU,
                                 StrId::STR_FRI, StrId::STR_SAT};
constexpr StrId HISTORY_MENU_TITLES[] = {StrId::STR_BOOK_HISTORY, StrId::STR_READING_METER};
constexpr StrId HISTORY_MENU_DESCRIPTIONS[] = {StrId::STR_BOOK_HISTORY_DESC, StrId::STR_READING_METER_DESC};
constexpr UIIcon HISTORY_MENU_ICONS[] = {UIIcon::Recent, UIIcon::Library};

std::string formatDuration(const uint32_t seconds) {
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  char value[24];
  snprintf(value, sizeof(value), "%lu:%02lu", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  return std::string(value);
}
}  // namespace

void RecentBooksActivity::loadRecentBooks() {
  recentBooks.clear();
  bookStatuses.clear();
  bookCacheStatuses.clear();
  bookReadingSeconds.clear();
  const auto& books = READING_HISTORY.getBooks();
  recentBooks.reserve(books.size());
  bookStatuses.reserve(books.size());
  bookCacheStatuses.reserve(books.size());
  bookReadingSeconds.reserve(books.size());

  for (const auto& book : books) {
    if (book.seconds == 0 || !Storage.exists(book.path.c_str())) continue;
    recentBooks.push_back({book.path, book.title, book.author, "", book.bookId});
    bookStatuses.push_back(getReadingStatus(book.path, "/.crosspoint", book.bookId));
    bookCacheStatuses.push_back(FsHelpers::hasEpubExtension(book.path)
                                    ? Epub(book.path, "/.crosspoint").getCacheGenerationStatus()
                                    : Epub::CacheGenerationStatus::NotGenerated);
    bookReadingSeconds.push_back(book.seconds);
  }
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Load data
  loadRecentBooks();

  selectorIndex = 0;
  menuIndex = 0;
  screen = Screen::Menu;
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
  bookStatuses.clear();
  bookCacheStatuses.clear();
  bookReadingSeconds.clear();
}

void RecentBooksActivity::loop() {
  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (screen == Screen::Menu) {
      screen = menuIndex == 0 ? Screen::Books : Screen::Meter;
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

  if (screen == Screen::Meter) return;

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
    renderer.drawCenteredText(UI_12_FONT_ID, contentTop + 4, tr(STR_READING_METER));
    const auto drawBookSummary = [this, &summary, pageWidth, contentTop, &metrics](const int topY) {
      const std::string bookCount = std::string(tr(STR_READING_METER_BOOKS)) + ": " +
                                    std::to_string(summary.bookCount) + tr(STR_READING_METER_BOOKS_UNIT);
      renderer.drawCenteredText(UI_10_FONT_ID, topY, bookCount.c_str());
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, topY + 42, tr(STR_READING_METER_TOP_BOOKS), true,
                        EpdFontFamily::BOLD);
      for (uint8_t index = 0; index < summary.topBookCount; ++index) {
        const auto& book = summary.topBooks[index];
        const std::string value = formatDuration(book.seconds);
        const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value.c_str());
        const int titleWidth = pageWidth - metrics.contentSidePadding * 2 - valueWidth - 18;
        const auto title = renderer.truncatedText(UI_10_FONT_ID, book.title.c_str(), titleWidth);
        const int y = topY + 77 + index * 38;
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

      const int graphLeft = metrics.contentSidePadding + 18;
      const int graphWidth = pageWidth - graphLeft * 2;
      const int graphTop = contentTop + 145;
      const int graphHeight = std::max(80, std::min(170, contentHeight - 215));
      const int baselineY = graphTop + graphHeight;
      uint32_t maximum = 0;
      for (const auto seconds : summary.recentDaySeconds) maximum = std::max(maximum, seconds);
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
  } else if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_BOOK_HISTORY));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, recentBooks.size(), selectorIndex,
        [this](int index) { return recentBooks[index].title; }, [this](int index) { return recentBooks[index].author; },
        [this](int index) { return UITheme::getFileIcon(recentBooks[index].path, bookStatuses[index]); },
        [this](int index) {
          return formatDuration(bookReadingSeconds[index]) +
                 (FsHelpers::hasEpubExtension(recentBooks[index].path) ? CACHE_STATUS_VALUE_SPACER : "");
        });

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
  const char* previousLabel = screen == Screen::Meter ? "" : tr(STR_DIR_UP);
  const char* nextLabel = screen == Screen::Meter ? "" : tr(STR_DIR_DOWN);
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, previousLabel, nextLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
