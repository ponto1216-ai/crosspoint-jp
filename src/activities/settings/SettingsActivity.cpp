#include "SettingsActivity.h"

#include <FontManager.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <cstdio>
#include <algorithm>

#include "AozoraActivity.h"
#include "ButtonRemapActivity.h"
#include "ClearCacheActivity.h"
#include "CrossPointSettings.h"
#include "DirectionSettingsActivity.h"
#include "DiagnosticsActivity.h"
#include "ReaderProfilesActivity.h"
#include "ReaderTestViewActivity.h"
#include "FontDownloadActivity.h"
#include "FontSelectActivity.h"
#include "FontSelectionActivity.h"
#include "GenerateAllCacheActivity.h"
#include "HalGPIO.h"
#include "HalRTC.h"
#include "LanguageSelectActivity.h"
#include "LineSpacingSelectionActivity.h"
#include "MappedInputManager.h"
#include "SdFirmwareUpdateActivity.h"
#include "SdCardFontGlobals.h"
#include "SettingsList.h"
#include "SettingsBackupActivity.h"
#include "StatusBarSettingsActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

const StrId SettingsActivity::categoryNames[MAX_CATEGORIES] = {
    StrId::STR_CAT_DISPLAY, StrId::STR_CAT_READER, StrId::STR_CAT_CONTROLS, StrId::STR_CAT_SYSTEM};

void SettingsActivity::enterCategory(const int categoryIndex) {
  if (categoryIndex < 0 || categoryIndex >= categoryCount) return;

  selectedCategoryIndex = categoryIndex;
  rebuildSettingsLists();
  selectedSettingIndex = 0;
}

bool SettingsActivity::currentSettingIsEditable() const {
  const int settingIndex = selectedSettingIndex - 1;
  if (settingIndex < 0 || settingIndex >= settingsCount) return false;
  const auto& setting = (*currentSettings)[settingIndex];
  if (setting.type == SettingType::TOGGLE || setting.type == SettingType::VALUE) return true;
  return setting.type == SettingType::ENUM && setting.nameId != StrId::STR_FONT_FAMILY;
}

const char* SettingsActivity::currentSettingDescription() const {
  const int settingIndex = selectedSettingIndex - 1;
  if (settingIndex < 0 || settingIndex >= settingsCount || currentSettings == nullptr) return "";

  switch ((*currentSettings)[settingIndex].action) {
    case SettingAction::RemapFrontButtons:
      return tr(STR_SETTINGS_DESC_REMAP_FRONT_BUTTONS);
    case SettingAction::CustomiseStatusBar:
      return tr(STR_SETTINGS_DESC_STATUS_BAR);
    case SettingAction::Network:
      return tr(STR_SETTINGS_DESC_NETWORK);
    case SettingAction::ClearCache:
      return tr(STR_SETTINGS_DESC_CLEAR_READING_CACHE);
    case SettingAction::SdFirmwareUpdate:
      return tr(STR_SETTINGS_DESC_SD_FIRMWARE_UPDATE);
    case SettingAction::Language:
      return tr(STR_SETTINGS_DESC_LANGUAGE);
    case SettingAction::DownloadFonts:
      return tr(STR_SETTINGS_DESC_DOWNLOAD_FONTS);
    case SettingAction::SelectUiFont:
      return tr(STR_SETTINGS_DESC_UI_FONT);
    case SettingAction::GenerateAllCache:
      return tr(STR_SETTINGS_DESC_GENERATE_ALL_CACHE);
    case SettingAction::HorizontalSettings:
      return tr(STR_SETTINGS_DESC_HORIZONTAL_SETTINGS);
    case SettingAction::VerticalSettings:
      return tr(STR_SETTINGS_DESC_VERTICAL_SETTINGS);
    case SettingAction::Diagnostics:
      return tr(STR_SETTINGS_DESC_DIAGNOSTICS);
    case SettingAction::ReaderProfiles:
      return tr(STR_SETTINGS_DESC_READER_PROFILES);
    case SettingAction::SettingsBackup:
      return tr(STR_SETTINGS_DESC_BACKUP);
    case SettingAction::ReaderTestView:
      return tr(STR_READER_TEST_VIEW_DESC);
    case SettingAction::AozoraBunko:
    case SettingAction::None:
      return "";
  }
  return "";
}

void SettingsActivity::rebuildSettingsLists() {
  displaySettings.clear();
  readerSettings.clear();
  controlsSettings.clear();
  systemSettings.clear();

  for (auto& setting : getSettingsList(&sdFontSystem.registry())) {
    if (setting.category == StrId::STR_NONE_OPT) continue;
    if (setting.category == StrId::STR_CAT_DISPLAY) {
      displaySettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_READER) {
      readerSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_CONTROLS) {
      controlsSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_SYSTEM) {
      systemSettings.push_back(setting);
    } else if (setting.category == StrId::STR_CAT_RTC) {
      // Keep the master toggle visible only on a board with a detected RTC;
      // hide its dependent calendar settings until the feature is enabled.
      if (!halRTC.isAvailable()) {
        continue;
      }
      if (!SETTINGS.rtcEnabled && setting.nameId != StrId::STR_RTC_ENABLED) {
        continue;
      }
      systemSettings.push_back(setting);
    }
  }

  // Append device-only ACTION items
  controlsSettings.insert(controlsSettings.begin(),
                          SettingInfo::Action(StrId::STR_REMAP_FRONT_BUTTONS, SettingAction::RemapFrontButtons));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_WIFI_NETWORKS, SettingAction::Network));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_CLEAR_READING_CACHE, SettingAction::ClearCache));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_GENERATE_ALL_CACHE, SettingAction::GenerateAllCache));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SD_FIRMWARE_UPDATE, SettingAction::SdFirmwareUpdate));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_LANGUAGE, SettingAction::Language));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_DIAGNOSTICS, SettingAction::Diagnostics));
  systemSettings.push_back(SettingInfo::Action(StrId::STR_SETTINGS_BACKUP, SettingAction::SettingsBackup));
  // Direction-specific settings submenus at the top
  readerSettings.insert(readerSettings.begin(),
                        SettingInfo::Action(StrId::STR_HORIZONTAL_SETTINGS, SettingAction::HorizontalSettings));
  readerSettings.insert(readerSettings.begin() + 1,
                        SettingInfo::Action(StrId::STR_VERTICAL_SETTINGS, SettingAction::VerticalSettings));
  // Profiles come before font management: this keeps save/restore next to the
  // reading settings it affects, while the following action covers font setup.
  readerSettings.insert(readerSettings.begin() + 2,
                        SettingInfo::Action(StrId::STR_READER_PROFILES, SettingAction::ReaderProfiles));
  readerSettings.insert(readerSettings.begin() + 3,
                        SettingInfo::Action(StrId::STR_DOWNLOAD_FONTS, SettingAction::DownloadFonts));
  readerSettings.insert(readerSettings.begin() + 4,
                        SettingInfo::Action(StrId::STR_READER_TEST_VIEW, SettingAction::ReaderTestView));
  readerSettings.push_back(SettingInfo::Action(StrId::STR_CUSTOMISE_STATUS_BAR, SettingAction::CustomiseStatusBar));

  // Update currentSettings pointer and count for the active category
  switch (selectedCategoryIndex) {
    case 0:
      currentSettings = &displaySettings;
      break;
    case 1:
      currentSettings = &readerSettings;
      break;
    case 2:
      currentSettings = &controlsSettings;
      break;
    case 3:
      currentSettings = &systemSettings;
      break;
    default:
      currentSettings = &systemSettings;
      break;
  }
  settingsCount = static_cast<int>(currentSettings->size());
}

void SettingsActivity::onEnter() {
  Activity::onEnter();

  categoryCount = MAX_CATEGORIES;

  // Initialize selection based on caller hint.
  if (initialCategoryIndex < 0 || initialCategoryIndex >= categoryCount) {
    selectedCategoryIndex = 0;
  } else {
    selectedCategoryIndex = initialCategoryIndex;
  }

  rebuildSettingsLists();

  if (initialSettingIndex < 0) {
    selectedSettingIndex = 0;
  } else if (initialSettingIndex > settingsCount) {
    selectedSettingIndex = settingsCount;
  } else {
    selectedSettingIndex = initialSettingIndex;
  }

  // Trigger first update
  requestUpdate();
}

void SettingsActivity::onExit() {
  Activity::onExit();

  UITheme::getInstance().reload();  // Re-apply theme in case it was changed
}

void SettingsActivity::loop() {
  if (skipNextButtonCheck) {
    const bool confirmCleared = !mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
                                !mappedInput.wasPressed(MappedInputManager::Button::Confirm);
    const bool backCleared = !mappedInput.isPressed(MappedInputManager::Button::Back) &&
                             !mappedInput.wasPressed(MappedInputManager::Button::Back);
    if (confirmCleared && backCleared) {
      skipNextButtonCheck = false;
    }
    return;
  }

  // Side buttons always switch category tabs.  Keep the ends fixed so an
  // accidental press cannot wrap from the first tab to the last (or vice versa).
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    enterCategory(selectedCategoryIndex - 1);
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    enterCategory(selectedCategoryIndex + 1);
    requestUpdate();
    return;
  }

  if (editingValue) {
    buttonNavigator.onPress({MappedInputManager::Button::Left}, [this] {
      changeCurrentSetting(-1);
      requestUpdate();
    });
    buttonNavigator.onPress({MappedInputManager::Button::Right}, [this] {
      changeCurrentSetting(1);
      requestUpdate();
    });
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      editingValue = false;
      requestUpdate();
    }
    return;
  }

  // Handle actions with early return
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedSettingIndex == 0) {
      // Confirm advances category tabs, wrapping after the final tab. Side
      // buttons remain the non-wrapping tab controls above.
      enterCategory(ButtonNavigator::nextIndex(selectedCategoryIndex, categoryCount));
      requestUpdate();
      return;
    } else {
      if (currentSettingIsEditable()) {
        editingValue = true;
      } else {
        changeCurrentSetting(1, true, true);
      }
      requestUpdate();
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (selectedSettingIndex > 0) {
      selectedSettingIndex = 0;
      requestUpdate();
    } else {
      SETTINGS.saveToFile();
      if (onGoHome) {
        onGoHome();
      } else {
        finish();
      }
    }
    return;
  }

  // Front directional buttons move between settings.  Once Confirm enters
  // edit mode, the same Left/Right buttons change the selected value above.
  buttonNavigator.onRelease({MappedInputManager::Button::Right}, [this] {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });

  buttonNavigator.onRelease({MappedInputManager::Button::Left}, [this] {
    selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });

  buttonNavigator.onContinuous({MappedInputManager::Button::Right}, [this] {
    selectedSettingIndex = ButtonNavigator::nextIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });

  buttonNavigator.onContinuous({MappedInputManager::Button::Left}, [this] {
    selectedSettingIndex = ButtonNavigator::previousIndex(selectedSettingIndex, settingsCount + 1);
    requestUpdate();
  });
}

void SettingsActivity::changeCurrentSetting(const int delta, const bool activateAction, const bool toggleValue) {
  int selectedSetting = selectedSettingIndex - 1;
  if (selectedSetting < 0 || selectedSetting >= settingsCount) {
    return;
  }

  const auto& setting = (*currentSettings)[selectedSetting];

  if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
    // Toggle the boolean value using the member pointer
    SETTINGS.*(setting.valuePtr) = toggleValue ? !(SETTINGS.*(setting.valuePtr)) : (delta < 0 ? 0 : 1);
    // Apply invert images change immediately
    if (setting.nameId == StrId::STR_INVERT_IMAGES) {
      renderer.setInvertImagesInDarkMode(SETTINGS.invertImages);
    }
    // RTCマスタートグル変更時はサブ設定の表示/非表示を更新
    if (setting.nameId == StrId::STR_RTC_ENABLED) {
      rebuildSettingsLists();
      // 選択位置をクランプ（サブ設定が消えた場合に備える）
      if (selectedSettingIndex > settingsCount) {
        selectedSettingIndex = settingsCount;
      }
    }
  } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
    // Calendar Position: skip when calendar is disabled
    if (setting.nameId == StrId::STR_SLEEP_CALENDAR_POSITION && SETTINGS.sleepCalendar == 0) {
      return;
    }
    // Font Size: skip when external font is selected (fixed bitmap size)
    if (setting.nameId == StrId::STR_FONT_SIZE && FontMgr.getSelectedIndex() >= 0) {
      return;
    }
    // Font Family: open FontSelectActivity (combined built-in + external fonts)
    if (setting.nameId == StrId::STR_FONT_FAMILY) {
      if (!activateAction) return;
      startActivityForResult(std::make_unique<FontSelectActivity>(
                                 renderer, mappedInput, FontSelectActivity::SelectMode::Reader, [this] { finish(); }),
                             [this](const ActivityResult&) {
                               SETTINGS.saveToFile();
                               rebuildSettingsLists();
                             });
      return;
    }
    const uint8_t currentValue = SETTINGS.*(setting.valuePtr);
    const int next = std::clamp(static_cast<int>(currentValue) + delta, 0,
                                static_cast<int>(setting.enumValues.size()) - 1);
    SETTINGS.*(setting.valuePtr) = static_cast<uint8_t>(next);

    // Apply dark mode change immediately (renderer needs explicit notification)
    if (setting.nameId == StrId::STR_COLOR_MODE) {
      renderer.setDarkMode(SETTINGS.colorMode == CrossPointSettings::COLOR_MODE::DARK_MODE);
    }
  } else if (setting.type == SettingType::ENUM && setting.valueGetter && setting.valueSetter) {
    if (setting.nameId == StrId::STR_FONT_FAMILY) {
      if (!activateAction) return;
      // Launch font selection submenu instead of cycling
      startActivityForResult(std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry()),
                             [this](const ActivityResult&) {
                               SETTINGS.saveToFile();
                               rebuildSettingsLists();
                             });
      return;
    }
    const uint8_t totalValues = setting.enumStringValues.empty()
                                    ? static_cast<uint8_t>(setting.enumValues.size())
                                    : static_cast<uint8_t>(setting.enumStringValues.size());
    const uint8_t cur = setting.valueGetter();
    const int next = std::clamp(static_cast<int>(cur) + delta, 0, static_cast<int>(totalValues) - 1);
    setting.valueSetter(static_cast<uint8_t>(next));
  } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
    // Line spacing uses a slider activity (0.8x-2.5x) for finer control.
    // Note: Line spacing settings have moved to DirectionSettings and are no longer in the
    // main settings list. This code path is retained for backward compatibility.
    if (setting.nameId == StrId::STR_LINE_SPACING_HORIZONTAL || setting.nameId == StrId::STR_LINE_SPACING_VERTICAL) {
      if (!activateAction) return;
      const bool isVertical = (setting.nameId == StrId::STR_LINE_SPACING_VERTICAL);
      uint8_t& target = SETTINGS.getDirectionSettings(isVertical).lineSpacing;
      startActivityForResult(std::make_unique<LineSpacingSelectionActivity>(
                                 renderer, mappedInput, static_cast<int>(target),
                                 [this, &target](const int selectedValue) {
                                   target = static_cast<uint8_t>(selectedValue);
                                   SETTINGS.saveToFile();
                                   finish();
                                 },
                                 [this] { finish(); }),
                             [this](const ActivityResult&) { requestUpdate(); });
      return;
    }

    const int currentValue = SETTINGS.*(setting.valuePtr);
    const int next = std::clamp(currentValue + delta * setting.valueRange.step,
                                static_cast<int>(setting.valueRange.min), static_cast<int>(setting.valueRange.max));
    SETTINGS.*(setting.valuePtr) = static_cast<uint8_t>(next);
  } else if (setting.type == SettingType::ACTION) {
    if (!activateAction) return;
    auto resultHandler = [this](const ActivityResult&) { SETTINGS.saveToFile(); };

    switch (setting.action) {
      case SettingAction::RemapFrontButtons:
        startActivityForResult(std::make_unique<ButtonRemapActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::CustomiseStatusBar:
        startActivityForResult(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::Network:
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false), resultHandler);
        break;
      case SettingAction::ClearCache:
        startActivityForResult(std::make_unique<ClearCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::GenerateAllCache:
        startActivityForResult(std::make_unique<GenerateAllCacheActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SdFirmwareUpdate:
        startActivityForResult(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::DownloadFonts:
        startActivityForResult(std::make_unique<FontDownloadActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::AozoraBunko:
        startActivityForResult(std::make_unique<AozoraActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) { SETTINGS.saveToFile(); });
        break;
      case SettingAction::Language:
        startActivityForResult(std::make_unique<LanguageSelectActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SelectUiFont:
        startActivityForResult(std::make_unique<FontSelectActivity>(
                                   renderer, mappedInput, FontSelectActivity::SelectMode::UI, [this] { finish(); }),
                               [this](const ActivityResult&) {
                                 SETTINGS.saveToFile();
                                 rebuildSettingsLists();
                               });
        break;
      case SettingAction::HorizontalSettings:
        startActivityForResult(std::make_unique<DirectionSettingsActivity>(renderer, mappedInput, false),
                               [this](const ActivityResult&) {
                                 skipNextButtonCheck = true;
                                 rebuildSettingsLists();
                                 requestUpdate();
                               });
        break;
      case SettingAction::VerticalSettings:
        startActivityForResult(std::make_unique<DirectionSettingsActivity>(renderer, mappedInput, true),
                               [this](const ActivityResult&) {
                                 skipNextButtonCheck = true;
                                 rebuildSettingsLists();
                                 requestUpdate();
                               });
        break;
      case SettingAction::Diagnostics:
        startActivityForResult(std::make_unique<DiagnosticsActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::ReaderProfiles:
        startActivityForResult(std::make_unique<ReaderProfilesActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::SettingsBackup:
        startActivityForResult(std::make_unique<SettingsBackupActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::ReaderTestView:
        startActivityForResult(std::make_unique<ReaderTestViewActivity>(renderer, mappedInput), resultHandler);
        break;
      case SettingAction::None:
        // Do nothing
        break;
    }
    // Results will be handled in the result handler; also avoids concurrent SD card access.
    return;
  } else {
    return;
  }

  SETTINGS.saveToFile();
}

void SettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool isPortraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;
  // X3 has one side button on each edge. Leave a compact gutter so labels and
  // values stay clear of the vertical button hints without wasting list width.
  constexpr int x3ClassicSettingsSideInset = 23;
  constexpr int x3LyraSettingsSideInset = 15;
  const bool isLyraTheme = SETTINGS.uiTheme != CrossPointSettings::UI_THEME::CLASSIC;
  const int listSideInset =
      gpio.deviceIsX3() ? (isLyraTheme ? x3LyraSettingsSideInset : x3ClassicSettingsSideInset) : 0;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding + hintGutterHeight, pageWidth, metrics.headerHeight},
                 tr(STR_SETTINGS_TITLE), CROSSPOINT_VERSION);

  std::vector<TabInfo> tabs;
  tabs.reserve(categoryCount);
  for (int i = 0; i < categoryCount; i++) {
    tabs.push_back({I18N.get(categoryNames[i]), selectedCategoryIndex == i});
  }
  GUI.drawTabBar(renderer,
                 Rect{0, metrics.topPadding + hintGutterHeight + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                 tabs, selectedSettingIndex == 0);

  const auto& settings = *currentSettings;
  const int listTop =
      metrics.topPadding + hintGutterHeight + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int helpTextHeight = renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing * 2 - helpTextHeight;
  GUI.drawList(
      renderer,
      Rect{listSideInset, listTop, pageWidth - listSideInset * 2, listBottom - listTop},
      settingsCount, selectedSettingIndex - 1,
      [&settings](int index) { return std::string(I18N.get(settings[index].nameId)); }, nullptr, nullptr,
      [&settings](int i) {
        const auto& setting = settings[i];
        std::string valueText = "";
        if (setting.type == SettingType::TOGGLE && setting.valuePtr != nullptr) {
          const bool value = SETTINGS.*(setting.valuePtr);
          valueText = value ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
        } else if (setting.type == SettingType::ENUM && setting.valuePtr != nullptr) {
          // Font Family: show external font name when selected
          if (setting.nameId == StrId::STR_FONT_FAMILY && FontMgr.getSelectedIndex() >= 0) {
            const FontInfo* info = FontMgr.getFontInfo(FontMgr.getSelectedIndex());
            valueText = info ? info->name : tr(STR_EXTERNAL_FONT);
            // Font Size: show actual pixel size when external font is active
          } else if (setting.nameId == StrId::STR_FONT_SIZE && FontMgr.getSelectedIndex() >= 0) {
            const FontInfo* info = FontMgr.getFontInfo(FontMgr.getSelectedIndex());
            valueText = info ? (std::to_string(info->size) + "pt") : "—";
          } else {
            const uint8_t value = SETTINGS.*(setting.valuePtr);
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::ENUM && setting.valueGetter) {
          const uint8_t value = setting.valueGetter();
          if (!setting.enumStringValues.empty() && value < setting.enumStringValues.size()) {
            valueText = setting.enumStringValues[value];
          } else if (value < setting.enumValues.size()) {
            valueText = I18N.get(setting.enumValues[value]);
          }
        } else if (setting.type == SettingType::VALUE && setting.valuePtr != nullptr) {
          if (setting.nameId == StrId::STR_LINE_SPACING) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.2fx", static_cast<float>(SETTINGS.*(setting.valuePtr)) / 100.0f);
            valueText = buf;
          } else {
            valueText = std::to_string(SETTINGS.*(setting.valuePtr));
          }
        } else if (setting.type == SettingType::ACTION && setting.nameId == StrId::STR_EXT_UI_FONT) {
          // Show current UI font name or "Built-in"
          if (FontMgr.isUiFontEnabled()) {
            const int idx = FontMgr.getUiSelectedIndex();
            const FontInfo* info = FontMgr.getFontInfo(idx);
            valueText = info ? info->name : tr(STR_EXTERNAL_FONT);
          } else {
            valueText = tr(STR_BUILTIN_DISABLED);
          }
        }
        return valueText;
      },
      editingValue);

  GUI.drawHelpText(renderer, Rect{listSideInset, listBottom + metrics.verticalSpacing, pageWidth - listSideInset * 2,
                                  helpTextHeight},
                   currentSettingDescription());

  // Draw help text
  const char* confirmLabel = tr(STR_SELECT);
  const char* previousLabel = tr(STR_PREVIOUS);
  const char* nextLabel = tr(STR_NEXT);
  if (editingValue) {
    confirmLabel = tr(STR_SELECT);
    previousLabel = tr(STR_PREVIOUS);
    nextLabel = tr(STR_NEXT);
  } else if (selectedSettingIndex == 0) {
    confirmLabel = "";
    previousLabel = tr(STR_PREVIOUS);
    nextLabel = tr(STR_NEXT);
  } else {
    const auto& setting = settings[selectedSettingIndex - 1];
    if (currentSettingIsEditable()) {
      confirmLabel = tr(STR_EDIT);
    } else {
      confirmLabel = tr(STR_SELECT);
    }
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, previousLabel, nextLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (selectedSettingIndex == 0) {
    GUI.drawSideButtonHints(renderer, tr(STR_PREVIOUS), tr(STR_NEXT));
  }

  // Always use standard refresh for settings screen
  renderer.displayBuffer();
}
