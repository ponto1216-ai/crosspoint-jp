#pragma once
#include "activities/Activity.h"
#include "sync/BatchImport.h"
#include "util/ButtonNavigator.h"
class BatchImportActivity final : public Activity {
  enum class Screen { Select, Review, Confirm, Running, Results };
  Screen screen = Screen::Select;
  // Parent review remains on the activity stack. Borrow its immutable metadata.
  const std::vector<yomuka::sync::ReceivedBook>& books;
  std::vector<size_t> rows;
  std::vector<uint8_t> selection;
  std::vector<size_t> reviewBooks;
  std::vector<std::string> reviewLines;
  size_t reviewIndex = 0;
  int reviewRowsPerPage() const;
  void beginReview(bool retry);
  void prepareReview();
  yomuka::sync::BatchImport batch;
  ButtonNavigator navigator;
  size_t cursor = 0;
  bool ignoreConfirm = true, ignoreBack = true, retrying = false;
  const char* resultText(const yomuka::sync::ImportOutcome&) const;

 public:
  BatchImportActivity(GfxRenderer&, MappedInputManager&, const std::vector<yomuka::sync::ReceivedBook>&);
  void loop() override;
  void render(RenderLock&&) override;
  bool supportsUiLandscape() const override { return true; }
  bool preventAutoSleep() override { return screen == Screen::Running || batch.blocked(); }
  bool skipLoopDelay() override { return screen == Screen::Running; }
};
