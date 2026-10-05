#pragma once
#include <Epub.h>

#include <functional>

#include "GfxRenderer.h"
#include "SnapshotExchange.h"
namespace yomuka::sync {
// Same receiver-side layout calculation for single and sequential imports.
struct SnapshotPositionProjector {
  GfxRenderer& renderer;
  std::shared_ptr<Epub> epub;
  ExchangeBook book;
  std::function<bool()> cancel = {};
  bool renderLocked = false;
  static bool project(const Progress&, const BookReaderSettings::Override*, Progress&, void*);
};
}  // namespace yomuka::sync
