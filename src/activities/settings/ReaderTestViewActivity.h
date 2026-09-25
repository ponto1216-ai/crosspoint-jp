#pragma once

#include <cstdint>

#include "BookReaderSettings.h"
#include "activities/Activity.h"

class ReaderTestViewActivity final : public Activity {
  bool vertical = false;
  bool rubyAdjustActive = false;
  bool rubyAdjustChanged = false;
  bool ignoreOpeningConfirmRelease = true;
  uint64_t bookFingerprint = 0;
  int8_t initialVertical = -1;

  void adjustRubyOffset(bool xAxis, int delta);
  void persistRubyAdjust();

 public:
  explicit ReaderTestViewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, uint64_t bookFingerprint = 0,
                                  int8_t initialVertical = -1)
      : Activity("ReaderTestView", renderer, mappedInput),
        bookFingerprint(bookFingerprint),
        initialVertical(initialVertical) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool supportsUiLandscape() const override { return true; }
};
