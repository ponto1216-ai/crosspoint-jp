#pragma once
#include "activities/Activity.h"
#include "sync/ReceivedBookScanner.h"
#include "util/ButtonNavigator.h"
class ReceivedReviewActivity final : public Activity {
  yomuka::sync::ReceivedBookScanner scanner;
  ButtonNavigator navigator;
  size_t cursor = 0;
  bool detail = false, ignoreConfirm = true, ignoreBack = true;
  unsigned long lastUpdate = 0;
  const char* status(const yomuka::sync::ReceivedBook& book) const;
  std::vector<std::string> details(const yomuka::sync::ReceivedBook& book) const;
  size_t detailCursor = 0;

 public:
  ReceivedReviewActivity(GfxRenderer& renderer, MappedInputManager& input, std::vector<std::string> files = {})
      : Activity("ReceivedReview", renderer, input), scanner(std::move(files)) {}
  void loop() override;
  void render(RenderLock&&) override;
  bool supportsUiLandscape() const override { return true; }
  bool preventAutoSleep() override { return !scanner.done(); }
  bool skipLoopDelay() override { return !scanner.done(); }
};
