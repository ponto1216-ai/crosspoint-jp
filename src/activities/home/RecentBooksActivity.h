#pragma once
#include <Epub.h>
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "ReadingHistoryStore.h"
#include "ReadingStatusHelper.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

class RecentBooksActivity final : public Activity {
 private:
  enum class Screen : uint8_t { Menu, Meter, Books };
  enum class MeterPage : uint8_t { Overview, Details };
  enum class DeleteMode : uint8_t { None, One, All };

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;
  size_t menuIndex = 0;
  Screen screen = Screen::Menu;
  MeterPage meterPage = MeterPage::Overview;
  bool booksLoaded = false;
  bool meterSummaryLoaded = false;
  DeleteMode deleteMode = DeleteMode::None;
  bool ignoreDeleteOpeningRelease = false;
  ReadingHistorySummary meterSummary;

  // Recent tab state
  std::vector<RecentBook> recentBooks;
  std::vector<ReadingStatus> bookStatuses;
  std::vector<Epub::CacheGenerationStatus> bookCacheStatuses;
  std::vector<bool> bookDetailsLoaded;
  std::vector<BookListStatusEntry> bookListStatusIndex;

  // Data loading
  void loadRecentBooks();
  void loadVisibleBookDetails(int pageStart, int pageItems);

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool supportsUiLandscape() const override { return true; }
};
