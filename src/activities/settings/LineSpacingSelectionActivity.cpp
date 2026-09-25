#include "LineSpacingSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "HalGPIO.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"

namespace {
constexpr int kSmallStep = 1;
constexpr int kLargeStep = 10;
}  // namespace

void LineSpacingSelectionActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  if (value < CrossPointSettings::LINE_SPACING_MIN) {
    value = CrossPointSettings::LINE_SPACING_MIN;
  } else if (value > CrossPointSettings::LINE_SPACING_MAX) {
    value = CrossPointSettings::LINE_SPACING_MAX;
  }
  requestUpdate();
}

void LineSpacingSelectionActivity::onExit() { ActivityWithSubactivity::onExit(); }

void LineSpacingSelectionActivity::adjustValue(const int delta) {
  value += delta;
  if (value < CrossPointSettings::LINE_SPACING_MIN) {
    value = CrossPointSettings::LINE_SPACING_MIN;
  } else if (value > CrossPointSettings::LINE_SPACING_MAX) {
    value = CrossPointSettings::LINE_SPACING_MAX;
  }
  requestUpdate();
}

void LineSpacingSelectionActivity::loop() {
  // This sub-page is opened from Settings on Confirm *press*.
  // Using release events here would consume the same key-up and immediately exit.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCancel();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    onSelect(value);
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustValue(-kSmallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustValue(kSmallStep); });
  // On X4 with the right side held upward, the physical pair is reversed on
  // screen. Keep the button labelled "+10" increasing the left-to-right bar.
  const bool reverseX4LandscapeStep =
      !gpio.deviceIsX3() && renderer.getOrientation() == GfxRenderer::Orientation::LandscapeCounterClockwise;
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ValueIncrease}, [this, reverseX4LandscapeStep] {
    adjustValue(reverseX4LandscapeStep ? -kLargeStep : kLargeStep);
  });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::ValueDecrease}, [this, reverseX4LandscapeStep] {
    adjustValue(reverseX4LandscapeStep ? kLargeStep : -kLargeStep);
  });
}

void LineSpacingSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto metrics = UITheme::getInstance().getMetrics();
  auto layout = UiLayout::from(renderer);
  if (layout.landscape) {
    if (gpio.deviceIsX3()) {
      constexpr int sideHintWidth = 54;
      layout.content.width -= sideHintWidth;
      if (!layout.frontHintsOnLeft) layout.content.x += sideHintWidth;
    } else {
      const int sideHintHeight = metrics.sideButtonHintsWidth;
      layout.content.height -= sideHintHeight;
      if (renderer.getOrientation() == GfxRenderer::Orientation::LandscapeCounterClockwise) {
        layout.content.y += sideHintHeight;
      }
    }
  }
  const bool isPortraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? (metrics.buttonHintsHeight + metrics.verticalSpacing) : 0;

  const int centerOffset = layout.content.x + layout.content.width / 2 - renderer.getScreenWidth() / 2;
  const int headerY = layout.content.y + metrics.topPadding + hintGutterHeight;
  GUI.drawHeader(renderer, Rect{layout.content.x, headerY, layout.content.width, metrics.headerHeight},
                 tr(STR_LINE_SPACING));

  char valueBuf[16];
  snprintf(valueBuf, sizeof(valueBuf), "%.2fx", static_cast<float>(value) / 100.0f);
  const std::string valueText = valueBuf;
  const int valueY = headerY + metrics.headerHeight + metrics.verticalSpacing * 2;
  renderer.drawCenteredTextOffset(UI_12_FONT_ID, valueY, valueText.c_str(), true, centerOffset, EpdFontFamily::BOLD);

  const int barWidth = std::min(360, std::max(80, layout.content.width - metrics.contentSidePadding * 4));
  constexpr int barHeight = 16;
  const int barX = layout.content.x + (layout.content.width - barWidth) / 2;
  const int barY = valueY + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing * 2;

  renderer.drawRect(barX, barY, barWidth, barHeight);

  const int range = CrossPointSettings::LINE_SPACING_MAX - CrossPointSettings::LINE_SPACING_MIN;
  const int normalized = value - CrossPointSettings::LINE_SPACING_MIN;
  const int fillWidth = (barWidth - 4) * normalized / range;
  if (fillWidth > 0) {
    renderer.fillRect(barX + 2, barY + 2, fillWidth, barHeight - 4);
  }

  const int knobX = barX + 2 + fillWidth - 2;
  renderer.fillRect(knobX, barY - 4, 4, barHeight + 8, true);

  renderer.drawCenteredTextOffset(SMALL_FONT_ID, barY + 30, tr(STR_PERCENT_STEP_HINT), true, centerOffset);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "-", "+");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // X4's physical side-button pair appears in the opposite left-to-right
  // order in landscape. Match the labels to the orientation-specific action.
  const bool reverseX4LandscapeHints = !gpio.deviceIsX3() && layout.landscape;
  GUI.drawSideButtonHints(renderer, reverseX4LandscapeHints ? "-10" : "+10", reverseX4LandscapeHints ? "+10" : "-10");

  renderer.displayBuffer();
}
