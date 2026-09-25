#include "EpubReaderFootnotesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "OrientationHelper.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"

void EpubReaderFootnotesActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(readerOrientation);
  mappedInput.setEffectiveOrientation(OrientationHelper::toInputOrientation(readerOrientation));
  selectedIndex = 0;
  requestUpdate();
}

void EpubReaderFootnotesActivity::onExit() { Activity::onExit(); }

void EpubReaderFootnotesActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(footnotes.size())) {
      setResult(FootnoteResult{footnotes[selectedIndex].href});
      finish();
    }
    return;
  }

  buttonNavigator.onNext([this] {
    if (!footnotes.empty()) {
      selectedIndex = (selectedIndex + 1) % footnotes.size();
      requestUpdate();
    }
  });

  buttonNavigator.onPrevious([this] {
    if (!footnotes.empty()) {
      selectedIndex = (selectedIndex - 1 + footnotes.size()) % footnotes.size();
      requestUpdate();
    }
  });
}

void EpubReaderFootnotesActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto layout = UiLayout::from(renderer);
  const bool portraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int topHintGutter = portraitInverted ? metrics.buttonHintsHeight + metrics.verticalSpacing : 0;
  const int titleY = layout.content.y + 15 + topHintGutter;
  const int listTop = layout.content.y + 60 + topHintGutter;
  const int bottomHints = layout.landscape ? 0 : metrics.buttonHintsHeight + metrics.verticalSpacing;
  const int listBottom = layout.content.y + layout.content.height - bottomHints;
  const int centerOffset = layout.content.x + layout.content.width / 2 - renderer.getScreenWidth() / 2;
  renderer.drawCenteredTextOffset(UI_12_FONT_ID, titleY, tr(STR_FOOTNOTES), true, centerOffset, EpdFontFamily::BOLD);

  if (footnotes.empty()) {
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, listTop + 30, tr(STR_NO_FOOTNOTES), true, centerOffset);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  constexpr int lineHeight = 36;
  const int marginLeft = layout.content.x + 20;

  const int visibleCount = std::max(1, (listBottom - listTop) / lineHeight);
  if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
  if (selectedIndex >= scrollOffset + visibleCount) scrollOffset = selectedIndex - visibleCount + 1;

  for (int i = scrollOffset; i < static_cast<int>(footnotes.size()) && i < scrollOffset + visibleCount; i++) {
    const int y = listTop + (i - scrollOffset) * lineHeight;
    const bool isSelected = (i == selectedIndex);

    if (isSelected) {
      renderer.fillRect(layout.content.x, y, layout.content.width - 1, lineHeight, true);
    }

    // Show footnote number and abbreviated href
    std::string label = footnotes[i].number;
    if (label.empty()) {
      label = tr(STR_LINK);
    }
    renderer.drawText(UI_10_FONT_ID, marginLeft, y + 4, label.c_str(), !isSelected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
