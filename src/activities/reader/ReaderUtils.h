#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include "MappedInputManager.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;

inline void applyOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

struct PageTurnResult {
  bool prev;
  bool next;
  bool fromTilt;
};

inline PageTurnResult detectPageTurn(const MappedInputManager& input, const bool reverseFrontButtons = false) {
  const bool usePress = !SETTINGS.longPressChapterSkip;
  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  const bool sideBack = usePress ? input.wasPressed(MappedInputManager::Button::PageBack)
                                 : input.wasReleased(MappedInputManager::Button::PageBack);
  const bool sideForward = usePress ? input.wasPressed(MappedInputManager::Button::PageForward)
                                    : input.wasReleased(MappedInputManager::Button::PageForward);
  const bool frontBack = usePress ? input.wasPressed(MappedInputManager::Button::Left)
                                  : input.wasReleased(MappedInputManager::Button::Left);
  const bool frontForward = usePress ? input.wasPressed(MappedInputManager::Button::Right)
                                     : input.wasReleased(MappedInputManager::Button::Right);
  const bool powerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                         input.wasReleased(MappedInputManager::Button::Power);
  const bool prev = tiltPrev || (reverseFrontButtons ? frontForward : frontBack) || sideBack;
  const bool next = tiltNext || powerTurn || (reverseFrontButtons ? frontBack : frontForward) || sideForward;
  return {prev, next, tiltPrev || tiltNext};
}

inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }
}

// the grayscale buffer. Only the content callback is re-rendered — status bars
}  // namespace ReaderUtils
