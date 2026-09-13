#pragma once
#include <Epub.h>
#include <I18n.h>

#include <string>
#include <functional>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderMenuActivity final : public Activity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    OPEN_READING_POSITION,
    OPEN_DISPLAY_LAYOUT,
    OPEN_BOOK_MANAGEMENT,
    OPEN_READING_BEHAVIOR,
    OPEN_TOOLS,
    SELECT_CHAPTER,
    BOOKMARKS,
    TOGGLE_BOOKMARK,
    FOOTNOTES,
    READER_SETTINGS,
    OPEN_GLOBAL_READER_SETTINGS,
    OPEN_BOOK_READER_SETTINGS,
    STYLE_FIRST_LINE_INDENT,
    STYLE_FONT_FAMILY,
    STYLE_FONT_SIZE,
    STYLE_LINE_SPACING,
    STYLE_INVERT_IMAGES,
    STYLE_STATUS_BAR,
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    RUBY_OFFSET,
    SCREENSHOT,
    DISPLAY_QR,
    DIAGNOSTICS,
    GO_HOME,
    GENERATE_CACHE,
    DELETE_CACHE,
    TILT_PAGE_TURN,
  };

  explicit EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                  const int currentPage, const int totalPages, const int bookProgressPercent,
                                  const uint8_t currentOrientation, const bool verticalMode, const bool hasBookmarks,
                                  Epub::CacheGenerationStatus cacheStatus,
                                  std::function<void()> onFirstLineIndentChanged = nullptr,
                                  std::function<void()> onInvertImagesChanged = nullptr,
                                  std::function<void()> onFontSizeChanged = nullptr);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool supportsLandscape() const override { return true; }

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  enum class MenuMode { Root, ReadingPosition, DisplayLayout, ReadingBehavior, BookManagement, Tools };

  static std::vector<MenuItem> buildMenuItems(MenuMode mode, bool hasBookmarks,
                                              Epub::CacheGenerationStatus cacheStatus);

  // Fixed menu layout
  std::vector<MenuItem> menuItems;
  MenuMode menuMode = MenuMode::Root;
  const bool hasBookmarks;
  const Epub::CacheGenerationStatus cacheStatus;
  int selectedIndex = 0;

  ButtonNavigator buttonNavigator;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  uint8_t selectedPageTurnOption = 0;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  const std::vector<const char*> pageTurnLabels = {I18N.get(StrId::STR_STATE_OFF), "1", "3", "6", "12"};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
  bool skipNextButtonCheck = true;
  bool verticalMode = false;
  bool layoutChanged = false;
  bool editingValue = false;
  std::function<void()> onFirstLineIndentChanged;
  std::function<void()> onInvertImagesChanged;
  std::function<void()> onFontSizeChanged;

  bool currentValueIsEditable() const;
  bool changeCurrentValue(int delta, bool toggleValue = false);
  std::string getMenuItemValue(MenuAction action) const;
  void openMenuMode(MenuMode mode);
};
