#pragma once
#include <Epub.h>
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "ReadingStatusHelper.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

class RecentBooksActivity final : public Activity {
 private:
  enum class Screen : uint8_t { Menu, Meter, Books };

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;
  size_t menuIndex = 0;
  Screen screen = Screen::Menu;

  // Recent tab state
  std::vector<RecentBook> recentBooks;
  std::vector<ReadingStatus> bookStatuses;
  std::vector<Epub::CacheGenerationStatus> bookCacheStatuses;
  std::vector<uint32_t> bookReadingSeconds;

  // Data loading
  void loadRecentBooks();

 public:
  explicit RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RecentBooks", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
