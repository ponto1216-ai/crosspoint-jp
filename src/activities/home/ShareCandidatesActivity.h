#pragma once

#include "activities/Activity.h"
#include "sync/BatchSnapshot.h"
#include "sync/ShareCandidateScanner.h"
#include "util/ButtonNavigator.h"

class ShareCandidatesActivity final : public Activity {
  yomuka::sync::ShareCandidateScanner scanner{false};
  ButtonNavigator navigator;
  size_t selected = 0;
  bool detail = false;
  bool ignoreOpeningConfirm = true;
  bool consumeConfirmRelease = false;
  bool consumeBackRelease = false;
  bool review = false, generating = false, results = false;
  std::vector<uint8_t> selection;
  yomuka::sync::BatchSnapshot batch;
  const bool returnToMenu;
  unsigned long lastProgressRender = 0, startedAt = 0;
  std::string unitSummary(const yomuka::sync::ShareCandidate& book) const;
  std::string actionMessage;

 public:
  ShareCandidatesActivity(GfxRenderer& renderer, MappedInputManager& input, bool returnToMenu = false)
      : Activity("ShareCandidates", renderer, input), returnToMenu(returnToMenu) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool supportsUiLandscape() const override { return true; }
  bool preventAutoSleep() override { return !scanner.done() || generating; }
  bool skipLoopDelay() override { return !scanner.done() || generating; }
};
