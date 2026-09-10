#include "TlsHeapReclaim.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <FontManager.h>
#include <GfxRenderer.h>
#include <Logging.h>

void reclaimHeapForTls(GfxRenderer& renderer, const char* tag) {
  const uint32_t heapBefore = ESP.getFreeHeap();
  const uint32_t blockBefore = ESP.getMaxAllocHeap();

  FontManager& fontManager = FontManager::getInstance();
  if (ExternalFont* uiFont = fontManager.getActiveUiFont()) uiFont->unload();
  if (ExternalFont* readerFont = fontManager.getActiveFont()) readerFont->unload();

  if (FontCacheManager* cacheManager = renderer.getFontCacheManager()) {
    // SD reader fonts can retain glyph, kern and advance-table allocations
    // after the reader has closed. Rebuild these lazily after the transfer so
    // HTTPS has the largest possible contiguous heap block on X3.
    cacheManager->releaseSdFontCaches();
  }

  LOG_DBG(tag, "Reclaimed for TLS: heap=%u->%u maxAlloc=%u->%u", heapBefore, ESP.getFreeHeap(), blockBefore,
          ESP.getMaxAllocHeap());
}
