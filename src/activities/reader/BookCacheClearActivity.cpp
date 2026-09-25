#include "BookCacheClearActivity.h"

#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "OrientationHelper.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"

void BookCacheClearActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(readerOrientation);
  mappedInput.setEffectiveOrientation(OrientationHelper::toInputOrientation(readerOrientation));
  requestUpdate();
}

void BookCacheClearActivity::onExit() { Activity::onExit(); }

void BookCacheClearActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto layout = UiLayout::from(renderer);
  const bool portraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int topHintGutter = portraitInverted ? metrics.buttonHintsHeight + metrics.verticalSpacing : 0;
  const int headerY = layout.content.y + metrics.topPadding + topHintGutter;
  const int centerX = layout.content.x + layout.content.width / 2 - renderer.getScreenWidth() / 2;
  const int centerY =
      headerY + metrics.headerHeight + (layout.content.y + layout.content.height - headerY - metrics.headerHeight) / 2;
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{layout.content.x, headerY, layout.content.width, metrics.headerHeight},
                 tr(STR_DELETE_CACHE));

  if (state == WARNING) {
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, centerY - 60, tr(STR_DELETE_BOOK_CACHE_WARNING_1), true, centerX,
                                    EpdFontFamily::BOLD);
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, centerY - 20, tr(STR_DELETE_BOOK_CACHE_WARNING_2), true, centerX);
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, centerY + 10, tr(STR_DELETE_BOOK_CACHE_WARNING_3), true, centerX);
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_DELETE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == CLEARING) {
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, centerY, tr(STR_CLEARING_CACHE), true, centerX);
  } else if (state == SUCCESS) {
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, centerY, tr(STR_CACHE_CLEARED), true, centerX, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, centerY, tr(STR_CLEAR_CACHE_FAILED), true, centerX,
                                    EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

void BookCacheClearActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      state = CLEARING;
      requestUpdateAndWait();
      state = epub && epub->clearCache() ? SUCCESS : FAILED;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }

  if ((state == SUCCESS || state == FAILED) && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = state != SUCCESS;
    setResult(std::move(result));
    finish();
  }
}
