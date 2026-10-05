#pragma once

#include "activities/Activity.h"
#include "sync/ShareCandidateScanner.h"
#include "util/ButtonNavigator.h"

class ShareCandidatesActivity final : public Activity {
  yomuka::sync::ShareCandidateScanner scanner;
  ButtonNavigator navigator;
  size_t selected = 0;
  bool detail = false;
  bool ignoreOpeningConfirm = true;
  const bool returnToMenu;
  unsigned long lastProgressRender = 0, startedAt = 0;
  std::string unitSummary(const yomuka::sync::ShareCandidate& book) const;

 public:
  ShareCandidatesActivity(GfxRenderer& renderer, MappedInputManager& input, bool returnToMenu = false)
      : Activity("ShareCandidates", renderer, input), returnToMenu(returnToMenu) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool supportsUiLandscape() const override { return true; }
  bool preventAutoSleep() override { return !scanner.done(); }
  bool skipLoopDelay() override { return !scanner.done(); }
};
