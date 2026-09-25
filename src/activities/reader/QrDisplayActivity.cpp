#include "QrDisplayActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "OrientationHelper.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"
#include "util/QrUtils.h"

void QrDisplayActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(readerOrientation);
  mappedInput.setEffectiveOrientation(OrientationHelper::toInputOrientation(readerOrientation));
  requestUpdate();
}

void QrDisplayActivity::onExit() { Activity::onExit(); }

void QrDisplayActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
    return;
  }
}

void QrDisplayActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto layout = UiLayout::from(renderer);
  const bool portraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int topHintGutter = portraitInverted ? metrics.buttonHintsHeight + metrics.verticalSpacing : 0;
  const int headerY = layout.content.y + metrics.topPadding + topHintGutter;

  GUI.drawHeader(renderer, Rect{layout.content.x, headerY, layout.content.width, metrics.headerHeight},
                 tr(STR_DISPLAY_QR), nullptr);

  const int bottomHints = layout.landscape ? 0 : metrics.buttonHintsHeight + metrics.verticalSpacing;
  const int startY = headerY + metrics.headerHeight + metrics.verticalSpacing;
  const int availableWidth = layout.content.width - metrics.contentSidePadding * 2;
  const int availableHeight = layout.content.y + layout.content.height - bottomHints - metrics.verticalSpacing - startY;

  const Rect qrBounds(layout.content.x + metrics.contentSidePadding, startY, availableWidth, availableHeight);
  QrUtils::drawQrCode(renderer, qrBounds, textPayload);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
