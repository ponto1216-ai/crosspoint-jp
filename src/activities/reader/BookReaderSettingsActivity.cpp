#include "BookReaderSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "BookReaderSettings.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/settings/ReaderTestViewActivity.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"

namespace {

constexpr uint16_t kFontFields = BookReaderSettings::DirectionFont;
constexpr uint16_t kSizeFields = BookReaderSettings::DirectionFontSize;
constexpr uint16_t kSpacingFields = BookReaderSettings::DirectionLineSpacing |
                                    BookReaderSettings::DirectionCharSpacing |
                                    BookReaderSettings::DirectionParagraphSpacing;
constexpr uint16_t kMarginFields =
    BookReaderSettings::DirectionMargin | BookReaderSettings::DirectionAlignment | BookReaderSettings::DirectionIndent;
constexpr uint16_t kRubyFields = BookReaderSettings::DirectionRubyEnabled | BookReaderSettings::DirectionRubyOffsetX |
                                 BookReaderSettings::DirectionRubyOffsetY;
constexpr uint16_t kTateChuYokoFields = BookReaderSettings::DirectionTateChuYokoDigits;

}  // namespace

void BookReaderSettingsActivity::onEnter() {
  Activity::onEnter();
  skipNextButtonCheck = true;
  requestUpdate();
}

void BookReaderSettingsActivity::selectCurrent() {
  const Item item = itemAtIndex(selectedIndex);
  if (item == Item::TestView) {
    startActivityForResult(
        std::make_unique<ReaderTestViewActivity>(renderer, mappedInput, fingerprint, verticalMode ? 1 : 0),
        [this](const ActivityResult&) { requestUpdate(); });
    return;
  }
  BookReaderSettings::Override value;
  bool success = BookReaderSettings::load(fingerprint, value);
  if (success && item == Item::SaveAll) {
    value = BookReaderSettings::captureAll(SETTINGS);
  } else if (success && item == Item::ClearAll) {
    value = BookReaderSettings::Override{};
  } else if (success) {
    const auto current = BookReaderSettings::captureAll(SETTINGS);
    auto& direction = verticalMode ? value.vertical : value.horizontal;
    const auto& currentDirection = verticalMode ? current.vertical : current.horizontal;
    auto toggleDirection = [&current](BookReaderSettings::DirectionOverride& target,
                                      const BookReaderSettings::DirectionOverride& source, const uint16_t fields) {
      if ((target.fields & fields) != 0) {
        target.fields &= ~fields;
      } else {
        target.values = source.values;
        target.fields |= fields;
      }
    };
    switch (item) {
      case Item::TestView:
        break;
      case Item::Font:
        toggleDirection(direction, currentDirection, kFontFields);
        break;
      case Item::Size:
        toggleDirection(direction, currentDirection, kSizeFields);
        break;
      case Item::Spacing:
        toggleDirection(direction, currentDirection, kSpacingFields);
        break;
      case Item::Margin:
        toggleDirection(direction, currentDirection, kMarginFields);
        break;
      case Item::Ruby:
        toggleDirection(direction, currentDirection, kRubyFields);
        break;
      case Item::TateChuYokoDigits:
        toggleDirection(direction, currentDirection, kTateChuYokoFields);
        break;
      case Item::WritingMode:
        if (value.fields & BookReaderSettings::WritingMode)
          value.fields &= ~BookReaderSettings::WritingMode;
        else {
          value.writingMode = current.writingMode;
          value.fields |= BookReaderSettings::WritingMode;
        }
        break;
      case Item::BookStyle:
        if (value.fields & BookReaderSettings::BookStyle)
          value.fields &= ~BookReaderSettings::BookStyle;
        else {
          value.bookStyle = current.bookStyle;
          value.fields |= BookReaderSettings::BookStyle;
        }
        break;
      case Item::SaveAll:
      case Item::ClearAll:
      case Item::Count:
        break;
    }
  }
  if (success) success = BookReaderSettings::save(fingerprint, value);
  const StrId labelId =
      success ? (item == Item::ClearAll ? StrId::STR_BOOK_SETTINGS_CLEARED : StrId::STR_BOOK_SETTINGS_SAVED)
              : StrId::STR_BOOK_SETTINGS_FAILED;
  resultText = I18N.get(labelId);
  if (success) setResult(MenuResult{static_cast<int>(item)});
  requestUpdate();
}

bool BookReaderSettingsActivity::isOverridden(const Item item) const {
  BookReaderSettings::Override value;
  if (!BookReaderSettings::load(fingerprint, value)) return false;
  const auto hasDirection = [](const BookReaderSettings::DirectionOverride& direction, const uint16_t fields) {
    return (direction.fields & fields) != 0;
  };
  const auto& direction = verticalMode ? value.vertical : value.horizontal;
  switch (item) {
    case Item::TestView:
      return false;
    case Item::Font:
      return hasDirection(direction, kFontFields);
    case Item::Size:
      return hasDirection(direction, kSizeFields);
    case Item::Spacing:
      return hasDirection(direction, kSpacingFields);
    case Item::Margin:
      return hasDirection(direction, kMarginFields);
    case Item::Ruby:
      return hasDirection(direction, kRubyFields);
    case Item::TateChuYokoDigits:
      return hasDirection(direction, kTateChuYokoFields);
    case Item::WritingMode:
      return value.fields & BookReaderSettings::WritingMode;
    case Item::BookStyle:
      return value.fields & BookReaderSettings::BookStyle;
    case Item::SaveAll:
      return BookReaderSettings::hasAnyField(value);
    case Item::ClearAll:
    case Item::Count:
      return false;
  }
  return false;
}

int BookReaderSettingsActivity::itemCount() const { return static_cast<int>(Item::Count) - (verticalMode ? 0 : 1); }

BookReaderSettingsActivity::Item BookReaderSettingsActivity::itemAtIndex(const int index) const {
  int itemIndex = index;
  if (!verticalMode && itemIndex >= static_cast<int>(Item::TateChuYokoDigits)) ++itemIndex;
  return static_cast<Item>(itemIndex);
}

StrId BookReaderSettingsActivity::itemLabel(const Item item) {
  static constexpr StrId kLabels[] = {
      StrId::STR_READER_TEST_VIEW,
      StrId::STR_FONT_FAMILY,
      StrId::STR_FONT_SIZE,
      StrId::STR_LINE_SPACING,
      StrId::STR_SCREEN_MARGIN,
      StrId::STR_BOOK_SETTINGS_RUBY,
      StrId::STR_TATE_CHU_YOKO_DIGITS,
      StrId::STR_BOOK_SETTINGS_WRITING_MODE,
      StrId::STR_BOOK_STYLE,
      StrId::STR_BOOK_SETTINGS_SAVE_CURRENT,
      StrId::STR_BOOK_SETTINGS_CLEAR,
  };
  return kLabels[static_cast<int>(item)];
}

void BookReaderSettingsActivity::loop() {
  if (skipNextButtonCheck) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        !mappedInput.wasReleased(MappedInputManager::Button::Confirm) &&
        !mappedInput.isPressed(MappedInputManager::Button::Back) &&
        !mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      skipNextButtonCheck = false;
    }
    return;
  }
  buttonNavigator.onPress({MappedInputManager::Button::Right}, [this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount());
    requestUpdate();
  });
  buttonNavigator.onPress({MappedInputManager::Button::Left}, [this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount());
    requestUpdate();
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    selectCurrent();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void BookReaderSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto layout = UiLayout::from(renderer);
  const bool portraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int topHintGutter = portraitInverted ? metrics.buttonHintsHeight + metrics.verticalSpacing : 0;
  const int headerY = layout.content.y + metrics.topPadding + topHintGutter;
  GUI.drawHeader(renderer, Rect{layout.content.x, headerY, layout.content.width, metrics.headerHeight},
                 tr(STR_BOOK_READER_SETTINGS));

  const int noteHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int noteY = headerY + metrics.headerHeight + metrics.verticalSpacing;
  const int centerOffset = layout.content.x + layout.content.width / 2 - renderer.getScreenWidth() / 2;
  renderer.drawCenteredTextOffset(UI_10_FONT_ID, noteY, tr(STR_BOOK_SETTINGS_NOTE), true, centerOffset);

  const int visibleItemCount = itemCount();
  const int listTop = noteY + noteHeight + metrics.verticalSpacing;
  const int bottomHints = layout.landscape ? 0 : metrics.buttonHintsHeight + metrics.verticalSpacing;
  const int resultHeight = resultText ? noteHeight + metrics.verticalSpacing : 0;
  const int listBottom =
      layout.content.y + layout.content.height - bottomHints - metrics.verticalSpacing - resultHeight;
  GUI.drawList(
      renderer, Rect{layout.content.x, listTop, layout.content.width, std::max(0, listBottom - listTop)},
      visibleItemCount, selectedIndex,
      [this](const int index) { return std::string(I18N.get(itemLabel(itemAtIndex(index)))); }, nullptr, nullptr,
      [this](const int index) {
        const Item item = itemAtIndex(index);
        return std::string(item == Item::TestView
                               ? tr(STR_BOOK_SETTINGS_PREVIEW)
                               : (isOverridden(item) ? tr(STR_BOOK_SETTINGS_THIS_BOOK) : tr(STR_BOOK_SETTINGS_GLOBAL)));
      },
      false);
  if (resultText)
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, listBottom + metrics.verticalSpacing, resultText, true,
                                    centerOffset);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_PREVIOUS), tr(STR_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
