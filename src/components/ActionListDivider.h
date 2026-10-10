#pragma once

#include <cstddef>

#include "components/UITheme.h"

// Separate list entries from a trailing action only when both share a page.
inline void drawActionListDivider(const GfxRenderer& renderer, const Rect& rect, int rowHeight,
                                  size_t firstAction, size_t selected) {
  if (rowHeight <= 0 || rect.height < rowHeight || firstAction == 0) return;
  const size_t pageItems = static_cast<size_t>(rect.height / rowHeight);
  const size_t actionRow = firstAction % pageItems;
  if (actionRow == 0 || selected / pageItems != firstAction / pageItems) return;
  const int y = rect.y + static_cast<int>(actionRow) * rowHeight - 4;
  renderer.drawLine(rect.x + UITheme::getInstance().getMetrics().listTextLeftInset, y, rect.x + rect.width - 30, y);
}
