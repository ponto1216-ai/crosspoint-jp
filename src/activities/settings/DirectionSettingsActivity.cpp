#include "DirectionSettingsActivity.h"

#include <FontManager.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "FontSelectionActivity.h"
#include "LineSpacingSelectionActivity.h"
#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"

DirectionSettingsActivity::DirectionSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     bool isVertical)
    : Activity("DirSettings", renderer, mappedInput), isVertical(isVertical) {}

void DirectionSettingsActivity::buildItems() {
  items.clear();
  items.reserve(13);

  // Font Family
  items.push_back({StrId::STR_FONT_FAMILY, Item::Type::FONT_FAMILY, nullptr, {}, {}});

  // Font Size
  items.push_back({StrId::STR_FONT_SIZE,
                   Item::Type::ENUM,
                   &DirectionSettings::fontSize,
                   {StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE, StrId::STR_X_LARGE},
                   {}});

  // Line Spacing opens its own detailed adjustment screen.
  items.push_back(
      {StrId::STR_LINE_SPACING, Item::Type::PRESET, &DirectionSettings::lineSpacing, {}, {}, {90, 120, 155, 185, 220}});

  // Character Spacing (vertical only — horizontal char spacing is not supported by renderer)
  if (isVertical) {
    items.push_back(
        {StrId::STR_CHAR_SPACING, Item::Type::PRESET, &DirectionSettings::charSpacing, {}, {}, {0, 8, 15, 30, 50}});
    items.push_back({StrId::STR_TATE_CHU_YOKO_DIGITS,
                     Item::Type::PRESET,
                     &DirectionSettings::tateChuYokoMaxDigits,
                     {},
                     {},
                     {2, 3}});
  }

  if (!isVertical) {
    items.push_back({StrId::STR_PARA_ALIGNMENT,
                     Item::Type::ENUM,
                     &DirectionSettings::paragraphAlignment,
                     {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT},
                     {}});
  }

  // Extra Paragraph Spacing
  items.push_back({StrId::STR_EXTRA_SPACING,
                   Item::Type::PRESET,
                   &DirectionSettings::extraParagraphSpacing,
                   {},
                   {},
                   {0, 1, 2, 3, 4}});

  // Hyphenation
  items.push_back({StrId::STR_HYPHENATION, Item::Type::TOGGLE, &DirectionSettings::hyphenationEnabled, {}, {}});

  // Screen Margin
  items.push_back(
      {StrId::STR_SCREEN_MARGIN, Item::Type::PRESET, &DirectionSettings::screenMargin, {}, {}, {5, 8, 10, 20, 40}});

  // First Line Indent
  items.push_back({StrId::STR_FIRST_LINE_INDENT, Item::Type::TOGGLE, &DirectionSettings::firstLineIndent, {}, {}});

  // Ruby (Furigana)
  items.push_back({StrId::STR_RUBY_ENABLED, Item::Type::TOGGLE, &DirectionSettings::rubyEnabled, {}, {}});
}

void DirectionSettingsActivity::onEnter() {
  Activity::onEnter();
  buildItems();
  requestUpdate();
}

bool DirectionSettingsActivity::currentItemIsEditable() const {
  return selectedIndex >= 0 && selectedIndex < static_cast<int>(items.size()) &&
         items[selectedIndex].type != Item::Type::FONT_FAMILY;
}

const char* DirectionSettingsActivity::currentItemDescription() const {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(items.size())) return "";
  switch (items[selectedIndex].nameId) {
    case StrId::STR_FONT_FAMILY:
      return tr(STR_READER_SETTING_DESC_FONT_FAMILY);
    case StrId::STR_FONT_SIZE:
      return tr(STR_READER_SETTING_DESC_FONT_SIZE);
    case StrId::STR_LINE_SPACING:
      return tr(STR_READER_SETTING_DESC_LINE_SPACING);
    case StrId::STR_CHAR_SPACING:
      return tr(STR_READER_SETTING_DESC_CHAR_SPACING);
    case StrId::STR_TATE_CHU_YOKO_DIGITS:
      return tr(STR_READER_SETTING_DESC_TATE_CHU_YOKO_DIGITS);
    case StrId::STR_PARA_ALIGNMENT:
      return I18N.get(isVertical ? StrId::STR_READER_SETTING_DESC_VERTICAL_ALIGNMENT
                                 : StrId::STR_READER_SETTING_DESC_ALIGNMENT);
    case StrId::STR_EXTRA_SPACING:
      return tr(STR_READER_SETTING_DESC_EXTRA_SPACING);
    case StrId::STR_HYPHENATION:
      return tr(STR_READER_SETTING_DESC_HYPHENATION);
    case StrId::STR_SCREEN_MARGIN:
      return tr(STR_READER_SETTING_DESC_SCREEN_MARGIN);
    case StrId::STR_FIRST_LINE_INDENT:
      return tr(STR_READER_SETTING_DESC_FIRST_LINE_INDENT);
    case StrId::STR_RUBY_ENABLED:
      return tr(STR_READER_SETTING_DESC_RUBY);
    default:
      return "";
  }
}

void DirectionSettingsActivity::onExit() {
  SETTINGS.saveToFile();
  Activity::onExit();
}

void DirectionSettingsActivity::changeCurrentItem(const int delta, const bool activateAction, const bool toggleValue) {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(items.size())) return;

  const auto& item = items[selectedIndex];

  switch (item.type) {
    case Item::Type::TOGGLE: {
      ds().*(item.valuePtr) = toggleValue ? !(ds().*(item.valuePtr)) : (delta < 0 ? 0 : 1);
      SETTINGS.saveToFile();
      break;
    }
    case Item::Type::ENUM: {
      // Font Size: skip when external font is selected (fixed bitmap size)
      if (item.nameId == StrId::STR_FONT_SIZE && FontMgr.getSelectedIndex() >= 0) {
        return;
      }
      const uint8_t cur = ds().*(item.valuePtr);
      const int next = std::clamp(static_cast<int>(cur) + delta, 0, static_cast<int>(item.enumValues.size()) - 1);
      ds().*(item.valuePtr) = static_cast<uint8_t>(next);
      SETTINGS.saveToFile();
      break;
    }
    case Item::Type::PRESET: {
      const uint8_t current = ds().*(item.valuePtr);
      int closest = 0;
      for (int i = 1; i < static_cast<int>(item.presetValues.size()); ++i) {
        if (std::abs(static_cast<int>(item.presetValues[i]) - current) <
            std::abs(static_cast<int>(item.presetValues[closest]) - current))
          closest = i;
      }
      const int next = std::clamp(closest + delta, 0, static_cast<int>(item.presetValues.size()) - 1);
      ds().*(item.valuePtr) = item.presetValues[next];
      SETTINGS.saveToFile();
      break;
    }
    case Item::Type::FONT_FAMILY: {
      if (!activateAction) return;
      startActivityForResult(
          std::make_unique<FontSelectionActivity>(renderer, mappedInput, &sdFontSystem.registry(), isVertical),
          [this](const ActivityResult&) {
            SETTINGS.saveToFile();
            skipNextButtonCheck = true;
            requestUpdate();
          });
      return;
    }
  }

  requestUpdate();
}

void DirectionSettingsActivity::loop() {
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
    buttonNavigator.onPress({MappedInputManager::Button::Left}, [this] { changeCurrentItem(-1); });
    buttonNavigator.onPress({MappedInputManager::Button::Right}, [this] { changeCurrentItem(1); });
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      editingValue = false;
      requestUpdate();
    }
    return;
  }

  buttonNavigator.onPress({MappedInputManager::Button::Right}, [this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(items.size()));
    requestUpdate();
  });

  buttonNavigator.onPress({MappedInputManager::Button::Left}, [this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(items.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (items[selectedIndex].nameId == StrId::STR_LINE_SPACING) {
      startActivityForResult(std::make_unique<LineSpacingSelectionActivity>(
                                 renderer, mappedInput, ds().lineSpacing,
                                 [this](const int selectedValue) {
                                   ds().lineSpacing = static_cast<uint8_t>(selectedValue);
                                   SETTINGS.saveToFile();
                                   finish();
                                 },
                                 [this] { finish(); }),
                             [this](const ActivityResult&) {
                               skipNextButtonCheck = true;
                               requestUpdate();
                             });
      return;
    }
    if (currentItemIsEditable()) {
      editingValue = true;
      requestUpdate();
    } else {
      changeCurrentItem(1, true, true);
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
}

void DirectionSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto layout = UiLayout::from(renderer);
  const bool isPortraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;

  // Header
  const char* title = isVertical ? tr(STR_VERTICAL_SETTINGS) : tr(STR_HORIZONTAL_SETTINGS);
  GUI.drawHeader(renderer,
                 Rect{layout.content.x, layout.content.y + metrics.topPadding + hintGutterHeight, layout.content.width,
                      metrics.headerHeight},
                 title, "");

  const int itemCount = static_cast<int>(items.size());
  const int helpTextHeight = renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  const int listTop =
      layout.content.y + metrics.topPadding + hintGutterHeight + metrics.headerHeight + metrics.verticalSpacing;
  const int bottomHints = layout.landscape ? 0 : metrics.buttonHintsHeight;
  const int listBottom =
      layout.content.y + layout.content.height - bottomHints - metrics.verticalSpacing * 2 - helpTextHeight;

  // List
  GUI.drawList(
      renderer, Rect{layout.content.x, listTop, layout.content.width, listBottom - listTop}, itemCount, selectedIndex,
      [this](int index) { return std::string(I18N.get(items[index].nameId)); }, nullptr, nullptr,
      [this](int i) -> std::string {
        const auto& item = items[i];
        if (item.nameId == StrId::STR_LINE_SPACING) {
          char valueBuf[16];
          snprintf(valueBuf, sizeof(valueBuf), "%.2fx", static_cast<float>(ds().lineSpacing) / 100.0f);
          return valueBuf;
        }
        switch (item.type) {
          case Item::Type::TOGGLE:
            return ds().*(item.valuePtr) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
          case Item::Type::ENUM: {
            // Font Size: show actual pixel size when external font is active
            if (item.nameId == StrId::STR_FONT_SIZE && FontMgr.getSelectedIndex() >= 0) {
              const FontInfo* info = FontMgr.getFontInfo(FontMgr.getSelectedIndex());
              return info ? (std::to_string(info->size) + "pt") : std::string("—");
            }
            const uint8_t val = ds().*(item.valuePtr);
            return std::string(I18N.get(item.enumValues[val]));
          }
          case Item::Type::PRESET: {
            const uint8_t value = ds().*(item.valuePtr);
            if (item.nameId == StrId::STR_TATE_CHU_YOKO_DIGITS) {
              return value >= 3 ? tr(STR_UP_TO_3_DIGITS) : tr(STR_UP_TO_2_DIGITS);
            }
            int closest = 0;
            for (int index = 1; index < static_cast<int>(item.presetValues.size()); ++index) {
              if (std::abs(static_cast<int>(item.presetValues[index]) - value) <
                  std::abs(static_cast<int>(item.presetValues[closest]) - value))
                closest = index;
            }
            constexpr StrId labels[] = {StrId::STR_MINIMUM, StrId::STR_TIGHT, StrId::STR_STANDARD, StrId::STR_WIDE,
                                        StrId::STR_MAXIMUM};
            return std::string(I18N.get(labels[closest]));
          }
          case Item::Type::FONT_FAMILY: {
            // Show current font name for this direction
            if (ds().sdFontFamilyName[0] != '\0') {
              return std::string(ds().sdFontFamilyName);
            }
            if (FontMgr.getSelectedIndex() >= 0) {
              const FontInfo* info = FontMgr.getFontInfo(FontMgr.getSelectedIndex());
              return info ? std::string(info->name) : tr(STR_EXTERNAL_FONT);
            }
            switch (ds().fontFamily) {
              case CrossPointSettings::NOTOSANS:
                return std::string(I18N.get(StrId::STR_NOTO_SANS));
              default:
                return std::string(I18N.get(StrId::STR_NOTO_SANS));
            }
          }
        }
        return "";
      },
      editingValue);

  GUI.drawHelpText(renderer,
                   Rect{layout.content.x, listBottom + metrics.verticalSpacing, layout.content.width, helpTextHeight},
                   currentItemDescription());

  // Button hints identify whether the selected item is being edited.
  const char* confirmLabel = tr(STR_SELECT);
  const char* previousLabel = tr(STR_PREVIOUS);
  const char* nextLabel = tr(STR_NEXT);
  if (!editingValue && currentItemIsEditable()) {
    confirmLabel = tr(STR_EDIT);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, previousLabel, nextLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
