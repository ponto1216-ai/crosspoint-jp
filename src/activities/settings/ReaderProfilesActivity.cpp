#include "ReaderProfilesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "../util/ConfirmationActivity.h"
#include "ReaderProfile.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"

namespace {

constexpr int kResetItemIndex = ReaderProfile::SLOT_COUNT * 2;

std::string itemLabel(const int index) {
  if (index == kResetItemIndex) return tr(STR_READER_SETTINGS_RESET);
  const bool isLoad = index >= ReaderProfile::SLOT_COUNT;
  const int slot = (index % ReaderProfile::SLOT_COUNT) + 1;
  return std::string(isLoad ? tr(STR_READER_PROFILE_LOAD) : tr(STR_READER_PROFILE_SAVE)) + " " + std::to_string(slot);
}

const char* resultLabel(const ReaderProfile::LoadResult result) {
  switch (result) {
    case ReaderProfile::LoadResult::Loaded:
      return tr(STR_READER_PROFILE_LOADED);
    case ReaderProfile::LoadResult::LoadedWithMissingFont:
      return tr(STR_READER_PROFILE_FONT_FALLBACK);
    case ReaderProfile::LoadResult::Missing:
      return tr(STR_READER_PROFILE_MISSING);
    case ReaderProfile::LoadResult::Invalid:
      return tr(STR_READER_PROFILE_INVALID);
    case ReaderProfile::LoadResult::BackupFailed:
      return tr(STR_READER_PROFILE_BACKUP_FAILED);
    case ReaderProfile::LoadResult::SaveFailed:
      return tr(STR_READER_PROFILE_SAVE_FAILED);
  }
  return tr(STR_READER_PROFILE_INVALID);
}

}  // namespace

void ReaderProfilesActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void ReaderProfilesActivity::selectCurrent() {
  if (selectedIndex == kResetItemIndex) {
    startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_READER_SETTINGS_RESET),
                                                                  tr(STR_READER_SETTINGS_RESET_CONFIRM)),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               resultText = ReaderProfile::resetToDefaults() ? tr(STR_READER_SETTINGS_RESET_DONE)
                                                                             : tr(STR_READER_PROFILE_SAVE_FAILED);
                             }
                             requestUpdate();
                           });
    return;
  }
  const bool isLoad = selectedIndex >= ReaderProfile::SLOT_COUNT;
  const uint8_t slot = static_cast<uint8_t>(selectedIndex % ReaderProfile::SLOT_COUNT);
  if (!isLoad) {
    resultText = ReaderProfile::save(slot) ? tr(STR_READER_PROFILE_SAVED) : tr(STR_READER_PROFILE_SAVE_FAILED);
    requestUpdate();
    return;
  }
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_READER_PROFILE_LOAD),
                                                                tr(STR_READER_PROFILE_LOAD_CONFIRM)),
                         [this, slot](const ActivityResult& result) {
                           if (!result.isCancelled) resultText = resultLabel(ReaderProfile::load(slot));
                           requestUpdate();
                         });
}

void ReaderProfilesActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    selectCurrent();
    return;
  }
  navigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, kItemCount);
    requestUpdate();
  });
  navigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, kItemCount);
    requestUpdate();
  });
}

void ReaderProfilesActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto layout = UiLayout::from(renderer);
  const bool portraitInverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int topHintGutter = portraitInverted ? metrics.buttonHintsHeight + metrics.verticalSpacing : 0;
  const int headerY = layout.content.y + metrics.topPadding + topHintGutter;
  GUI.drawHeader(renderer, Rect{layout.content.x, headerY, layout.content.width, metrics.headerHeight},
                 tr(STR_READER_PROFILES));
  const int top = headerY + metrics.headerHeight + metrics.verticalSpacing;
  const int bottomHints = layout.landscape ? 0 : metrics.buttonHintsHeight + metrics.verticalSpacing;
  const int resultHeight = resultText.empty() ? 0 : renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing;
  const int height = layout.content.y + layout.content.height - top - bottomHints - resultHeight;
  GUI.drawList(renderer, Rect{layout.content.x, top, layout.content.width, height}, kItemCount, selectedIndex,
               [](const int index) { return itemLabel(index); });
  if (!resultText.empty()) {
    const int centerOffset = layout.content.x + layout.content.width / 2 - renderer.getScreenWidth() / 2;
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, top + height, resultText.c_str(), true, centerOffset);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
