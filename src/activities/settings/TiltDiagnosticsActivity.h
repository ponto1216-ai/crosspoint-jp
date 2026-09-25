#pragma once

#include <HalTiltSensor.h>

#include <cstdint>
#include <string>

#include "activities/Activity.h"

class TiltDiagnosticsActivity final : public Activity {
 public:
  TiltDiagnosticsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TiltDiagnostics", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool supportsUiLandscape() const override { return true; }

 private:
  enum class Page : uint8_t { Test, Details };
  enum class SaveResult : uint8_t { None, Saved, Failed };

  Page page = Page::Test;
  SaveResult saveResult = SaveResult::None;
  uint32_t lastRenderRequestMs = 0;
  std::string savedReportPath;

  bool saveReport();
  const char* recommendation() const;
  void drawWrapped(const char* text, int x, int& y, int width, int lineHeight, int maxLines = 3);
};
