#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool verticalMode, const bool hasBookmarks,
                                               const Epub::CacheGenerationStatus cacheStatus,
                                               std::function<void()> onFirstLineIndentChanged,
                                               std::function<void()> onInvertImagesChanged,
                                               std::function<void()> onFontSizeChanged)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(MenuMode::Root, hasBookmarks, cacheStatus)),
      hasBookmarks(hasBookmarks),
      cacheStatus(cacheStatus),
      title(title),
      pendingOrientation(currentOrientation),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent),
      verticalMode(verticalMode),
      onFirstLineIndentChanged(std::move(onFirstLineIndentChanged)),
      onInvertImagesChanged(std::move(onInvertImagesChanged)),
      onFontSizeChanged(std::move(onFontSizeChanged)) {}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMenuItems(
    const MenuMode mode, const bool hasBookmarks, const Epub::CacheGenerationStatus cacheStatus) {
  std::vector<MenuItem> items;
  switch (mode) {
    case MenuMode::Root:
      items = {{MenuAction::OPEN_READING_POSITION, StrId::STR_READING_POSITION},
               {MenuAction::OPEN_DISPLAY_LAYOUT, StrId::STR_BOOK_DISPLAY_SETTINGS},
               {MenuAction::OPEN_READING_BEHAVIOR, StrId::STR_READING_BEHAVIOR},
               {MenuAction::OPEN_BOOK_MANAGEMENT, StrId::STR_BOOK_CACHE},
               {MenuAction::OPEN_TOOLS, StrId::STR_READER_TOOLS},
               {MenuAction::READER_SETTINGS, StrId::STR_READER_SETTINGS},
               {MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON}};
      break;
    case MenuMode::ReadingPosition:
      items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
      if (hasBookmarks) items.push_back({MenuAction::BOOKMARKS, StrId::STR_BOOKMARKS});
      items.push_back({MenuAction::TOGGLE_BOOKMARK, StrId::STR_TOGGLE_BOOKMARK});
      items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
      break;
    case MenuMode::DisplayLayout:
      items = {{MenuAction::STYLE_FONT_FAMILY, StrId::STR_FONT_FAMILY},
               {MenuAction::STYLE_FONT_SIZE, StrId::STR_FONT_SIZE},
               {MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION},
               {MenuAction::STYLE_LINE_SPACING, StrId::STR_LINE_SPACING},
               {MenuAction::STYLE_FIRST_LINE_INDENT, StrId::STR_FIRST_LINE_INDENT},
               {MenuAction::RUBY_OFFSET, StrId::STR_RUBY_OFFSET_ADJUST},
               {MenuAction::STYLE_INVERT_IMAGES, StrId::STR_INVERT_IMAGES}};
      break;
    case MenuMode::BookManagement:
      if (cacheStatus != Epub::CacheGenerationStatus::Complete) {
        const StrId label = cacheStatus == Epub::CacheGenerationStatus::Resumable
                                ? StrId::STR_GENERATE_REMAINING_BOOK_CACHE
                                : StrId::STR_GENERATE_BOOK_CACHE;
        items.push_back({MenuAction::GENERATE_CACHE, label});
      }
      items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
      break;
    case MenuMode::ReadingBehavior:
      items = {{MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN},
               {MenuAction::TILT_PAGE_TURN, StrId::STR_TILT_PAGE_TURN}};
      break;
    case MenuMode::Tools:
      items = {{MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON},
               {MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR},
               {MenuAction::DIAGNOSTICS, StrId::STR_DIAGNOSTICS}};
      break;
  }
  return items;
}

void EpubReaderMenuActivity::openMenuMode(const MenuMode mode) {
  menuMode = mode;
  menuItems = buildMenuItems(menuMode, hasBookmarks, cacheStatus);
  selectedIndex = 0;
  editingValue = false;
  requestUpdate();
}

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  skipNextButtonCheck = true;
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() { Activity::onExit(); }

void EpubReaderMenuActivity::loop() {
  if (skipNextButtonCheck) {
    const bool confirmCleared = !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
                                !mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    const bool backCleared = !mappedInput.isPressed(MappedInputManager::Button::Back) &&
                             !mappedInput.wasReleased(MappedInputManager::Button::Back);
    if (confirmCleared && backCleared) {
      skipNextButtonCheck = false;
    }
    return;
  }

  if (editingValue) {
    buttonNavigator.onPress({MappedInputManager::Button::Left}, [this] {
      if (changeCurrentValue(-1)) requestUpdate();
    });
    buttonNavigator.onPress({MappedInputManager::Button::Right}, [this] {
      if (changeCurrentValue(1)) requestUpdate();
    });
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      editingValue = false;
      requestUpdate();
    }
    return;
  }

  // Front and side buttons both move through the menu. Values remain bound to
  // Front Left/Right after Confirm enters edit mode.
  buttonNavigator.onPress({MappedInputManager::Button::Right, MappedInputManager::Button::Down}, [this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  buttonNavigator.onPress({MappedInputManager::Button::Left, MappedInputManager::Button::Up}, [this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto selectedAction = menuItems[selectedIndex].action;
    if (selectedAction == MenuAction::OPEN_READING_POSITION) {
      openMenuMode(MenuMode::ReadingPosition);
      return;
    }
    if (selectedAction == MenuAction::OPEN_DISPLAY_LAYOUT) {
      openMenuMode(MenuMode::DisplayLayout);
      return;
    }
    if (selectedAction == MenuAction::OPEN_BOOK_MANAGEMENT) {
      openMenuMode(MenuMode::BookManagement);
      return;
    }
    if (selectedAction == MenuAction::OPEN_READING_BEHAVIOR) {
      openMenuMode(MenuMode::ReadingBehavior);
      return;
    }
    if (selectedAction == MenuAction::OPEN_TOOLS) {
      openMenuMode(MenuMode::Tools);
      return;
    }
    if (currentValueIsEditable()) {
      editingValue = true;
      requestUpdate();
      return;
    }
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      // Cycle orientation preview locally; actual rotation happens on menu exit.
      pendingOrientation = (pendingOrientation + 1) % orientationLabels.size();
      requestUpdate();
      return;
    }

    setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, selectedPageTurnOption, layoutChanged});
    finish();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (menuMode != MenuMode::Root) {
      openMenuMode(MenuMode::Root);
      return;
    }
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{-1, pendingOrientation, selectedPageTurnOption, layoutChanged};
    setResult(std::move(result));
    finish();
    return;
  }
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  // Landscape orientation: button hints are drawn along a vertical edge, so we
  // reserve a horizontal gutter to prevent overlap with menu content.
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  // Inverted portrait: button hints appear near the logical top, so we reserve
  // vertical space to keep the header and list clear.
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  constexpr int landscapeHintGutterWidth = 100;
  constexpr int landscapeSideHintGutterWidth = 54;
  const bool isLandscape = isLandscapeCw || isLandscapeCcw;
  const bool isX3Portrait = gpio.deviceIsX3() && !isLandscape;
  const int frontHintGutterWidth = isLandscape ? landscapeHintGutterWidth : 0;
  // X3 places its two side-button hints on the left and right edges in portrait.
  // Keep menu labels clear of the 30 px hint strips.
  const int sideHintGutterWidth = isLandscape ? landscapeSideHintGutterWidth : (isX3Portrait ? 30 : 0);
  // Front hints and side hints occupy opposite edges in landscape.
  const int contentX = isLandscapeCw ? frontHintGutterWidth : sideHintGutterWidth;
  const int contentWidth = pageWidth - frontHintGutterWidth - sideHintGutterWidth - (isX3Portrait ? 30 : 0);
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;

  // Title
  const std::string truncTitle =
      renderer.truncatedText(UI_12_FONT_ID, title.c_str(), contentWidth - 40, EpdFontFamily::BOLD);
  // Manual centering so we can respect the content gutter.
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, truncTitle.c_str(), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, truncTitle.c_str(), true, EpdFontFamily::BOLD);

  // Progress summary
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, 45 + contentY, progressLine.c_str());

  // Menu Items
  const int startY = 75 + contentY;
  constexpr int lineHeight = 30;
  constexpr int valueRightMargin = 32;
  const int footerHeight = UITheme::getInstance().getMetrics().buttonHintsHeight;
  const int availableHeight = std::max(lineHeight, renderer.getScreenHeight() - startY - footerHeight - 5);
  const int visibleCount = std::max(1, availableHeight / lineHeight);
  const int maxFirstVisible = std::max(0, static_cast<int>(menuItems.size()) - visibleCount);
  const int firstVisible = std::clamp(selectedIndex - visibleCount + 1, 0, maxFirstVisible);
  const int lastVisible = std::min(static_cast<int>(menuItems.size()), firstVisible + visibleCount);

  for (int i = firstVisible; i < lastVisible; ++i) {
    const int displayY = startY + ((i - firstVisible) * lineHeight);
    const bool isSelected = i == selectedIndex;

    if (isSelected) {
      // Highlight only the content area so we don't paint over hint gutters.
      renderer.fillRect(contentX, displayY, contentWidth - 1, lineHeight, true);
    }

    renderer.drawText(UI_10_FONT_ID, contentX + 20, displayY, I18N.get(menuItems[i].labelId), !isSelected);

    const std::string value = getMenuItemValue(menuItems[i].action);
    if (!value.empty()) {
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, value.c_str());
      renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - valueRightMargin - width, displayY, value.c_str(),
                        !isSelected);
    }

    if (menuItems[i].action == MenuAction::AUTO_PAGE_TURN) {
      // Render current page turn value on the right edge of the content area.
      const auto pageTurnValue = pageTurnLabels[selectedPageTurnOption];
      const auto pageTurnWidth = renderer.getTextWidth(UI_10_FONT_ID, pageTurnValue);
      renderer.drawText(UI_10_FONT_ID, contentX + contentWidth - valueRightMargin - pageTurnWidth, displayY,
                        pageTurnValue,
                        !isSelected);
    }
  }

  // Footer / Hints
  const auto selectedAction = menuItems[selectedIndex].action;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), editingValue ? tr(STR_SELECT)
                                                                        : (currentValueIsEditable() ? tr(STR_EDIT) : tr(STR_SELECT)),
                                            tr(STR_PREVIOUS), tr(STR_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (!editingValue) {
    GUI.drawSideButtonHints(renderer, tr(STR_PREVIOUS), tr(STR_NEXT));
  }

  renderer.displayBuffer();
}

std::string EpubReaderMenuActivity::getMenuItemValue(const MenuAction action) const {
  switch (action) {
    case MenuAction::STYLE_FONT_FAMILY: {
      const auto& settings = SETTINGS.getDirectionSettings(verticalMode);
      if (settings.sdFontFamilyName[0] != '\0') return std::string(settings.sdFontFamilyName);
      return std::string(I18N.get(StrId::STR_NOTO_SANS));
    }
    case MenuAction::STYLE_FONT_SIZE: {
      static constexpr StrId sizeLabels[] = {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE,
                                             StrId::STR_X_LARGE};
      const uint8_t size = std::min<uint8_t>(SETTINGS.getDirectionSettings(verticalMode).fontSize,
                                             CrossPointSettings::EXTRA_LARGE);
      return std::string(I18N.get(sizeLabels[size]));
    }
    case MenuAction::ROTATE_SCREEN:
      return std::string(I18N.get(orientationLabels[pendingOrientation]));
    case MenuAction::STYLE_FIRST_LINE_INDENT:
      return SETTINGS.getDirectionSettings(verticalMode).firstLineIndent ? std::string(tr(STR_STATE_ON))
                                                                         : std::string(tr(STR_STATE_OFF));
    case MenuAction::STYLE_INVERT_IMAGES:
      return SETTINGS.invertImages ? std::string(tr(STR_STATE_ON)) : std::string(tr(STR_STATE_OFF));
    case MenuAction::TILT_PAGE_TURN:
      return SETTINGS.tiltPageTurn ? std::string(tr(STR_STATE_ON)) : std::string(tr(STR_STATE_OFF));
    case MenuAction::STYLE_LINE_SPACING: {
      const uint8_t spacing = SETTINGS.getDirectionSettings(verticalMode).lineSpacing;
      char valueBuf[16];
      snprintf(valueBuf, sizeof(valueBuf), "%.2fx", static_cast<float>(spacing) / 100.0f);
      return valueBuf;
    }
    default:
      return "";
  }
}

bool EpubReaderMenuActivity::currentValueIsEditable() const {
  const auto action = menuItems[selectedIndex].action;
  return action == MenuAction::STYLE_FIRST_LINE_INDENT || action == MenuAction::STYLE_INVERT_IMAGES ||
         action == MenuAction::STYLE_FONT_SIZE || action == MenuAction::ROTATE_SCREEN || action == MenuAction::AUTO_PAGE_TURN ||
         action == MenuAction::TILT_PAGE_TURN;
}

bool EpubReaderMenuActivity::changeCurrentValue(const int delta, const bool toggleValue) {
  const auto action = menuItems[selectedIndex].action;
  switch (action) {
    case MenuAction::STYLE_FIRST_LINE_INDENT: {
      auto& value = SETTINGS.getDirectionSettings(verticalMode).firstLineIndent;
      value = toggleValue ? !value : (delta < 0 ? 0 : 1);
      if (onFirstLineIndentChanged) {
        onFirstLineIndentChanged();
      } else {
        SETTINGS.saveToFile();
      }
      layoutChanged = true;
      return true;
    }
    case MenuAction::STYLE_INVERT_IMAGES:
      SETTINGS.invertImages = toggleValue ? !SETTINGS.invertImages : (delta < 0 ? 0 : 1);
      if (onInvertImagesChanged) {
        onInvertImagesChanged();
      } else {
        SETTINGS.saveToFile();
      }
      return true;
    case MenuAction::STYLE_FONT_SIZE: {
      auto& value = SETTINGS.getDirectionSettings(verticalMode).fontSize;
      const uint8_t next = static_cast<uint8_t>(std::clamp(static_cast<int>(value) + delta,
                                                           static_cast<int>(CrossPointSettings::SMALL),
                                                           static_cast<int>(CrossPointSettings::EXTRA_LARGE)));
      if (next == value) return false;
      value = next;
      if (onFontSizeChanged) {
        onFontSizeChanged();
      } else {
        SETTINGS.saveToFile();
      }
      layoutChanged = true;
      return true;
    }
    case MenuAction::TILT_PAGE_TURN:
      SETTINGS.tiltPageTurn = toggleValue ? (SETTINGS.tiltPageTurn ? CrossPointSettings::TILT_OFF
                                                                    : CrossPointSettings::TILT_NORMAL)
                                           : static_cast<uint8_t>(std::clamp(
                                                 static_cast<int>(SETTINGS.tiltPageTurn) + delta,
                                                 static_cast<int>(CrossPointSettings::TILT_OFF),
                                                 static_cast<int>(CrossPointSettings::TILT_NVERTED)));
      SETTINGS.saveToFile();
      return true;
    case MenuAction::ROTATE_SCREEN:
      pendingOrientation = static_cast<uint8_t>(std::clamp(static_cast<int>(pendingOrientation) + delta, 0,
                                                            static_cast<int>(orientationLabels.size()) - 1));
      return true;
    case MenuAction::AUTO_PAGE_TURN:
      selectedPageTurnOption = static_cast<uint8_t>(std::clamp(static_cast<int>(selectedPageTurnOption) + delta, 0,
                                                                static_cast<int>(pageTurnLabels.size()) - 1));
      return true;
    default:
      return false;
  }
}
