#pragma once

#include <algorithm>

#include "components/UITheme.h"

// A moving segment shows activity without implying a known file count or percentage.
inline void drawIndeterminateProgress(const GfxRenderer& renderer, const Rect& rect, unsigned frame) {
  if (rect.width < 8 || rect.height < 5) return;
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
  const int innerWidth = rect.width - 4;
  const int segmentWidth = std::max(4, innerWidth / 4);
  const int travel = innerWidth - segmentWidth;
  const unsigned phase = frame % 8;
  const int offset = travel * static_cast<int>(phase <= 4 ? phase : 8 - phase) / 4;
  renderer.fillRect(rect.x + 2 + offset, rect.y + 2, segmentWidth, rect.height - 4);
}
