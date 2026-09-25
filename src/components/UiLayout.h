#pragma once

#include <GfxRenderer.h>

#include "components/UITheme.h"

struct UiLayout {
  Rect screen;
  Rect content;
  bool landscape = false;
  bool frontHintsOnLeft = false;
  int frontHintGutter = 0;

  static UiLayout from(const GfxRenderer& renderer, const bool reserveButtonHints = true) {
    UiLayout result;
    result.screen = Rect{0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()};
    const auto orientation = renderer.getOrientation();
    result.landscape = orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                       orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
    result.frontHintsOnLeft = orientation == GfxRenderer::Orientation::LandscapeClockwise;
    result.frontHintGutter =
        result.landscape && reserveButtonHints ? UITheme::getInstance().getMetrics().landscapeButtonHintsWidth : 0;
    result.content = result.screen;
    if (result.frontHintGutter > 0) {
      result.content.width -= result.frontHintGutter;
      if (result.frontHintsOnLeft) result.content.x += result.frontHintGutter;
    }
    return result;
  }
};
