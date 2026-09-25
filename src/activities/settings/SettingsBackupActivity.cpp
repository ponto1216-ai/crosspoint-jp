#include "SettingsBackupActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>

#include "../util/ConfirmationActivity.h"
#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"

namespace {
constexpr const char* kDirectory = "/.crosspoint/backups";
constexpr const char* kBackup = "/.crosspoint/backups/yomuka-settings.json";
constexpr const char* kBeforeImport = "/.crosspoint/backups/yomuka-settings-before-import.json";
constexpr int kItemCount = 2;
}  // namespace

void SettingsBackupActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

bool SettingsBackupActivity::saveBackup() const {
  return Storage.ensureDirectoryExists(kDirectory) && JsonSettingsIO::saveSettings(SETTINGS, kBackup);
}

bool SettingsBackupActivity::restoreBackup() const {
  if (!Storage.ensureDirectoryExists(kDirectory) || !Storage.exists(kBackup)) return false;
  const String json = Storage.readFile(kBackup);
  if (json.isEmpty() || !JsonSettingsIO::saveSettings(SETTINGS, kBeforeImport)) return false;
  bool needsResave = false;
  return JsonSettingsIO::loadSettings(SETTINGS, json.c_str(), &needsResave) && SETTINGS.saveToFile();
}

void SettingsBackupActivity::selectCurrent() {
  if (selectedIndex == 0) {
    resultText = saveBackup() ? tr(STR_SETTINGS_BACKUP_SAVED) : tr(STR_SETTINGS_BACKUP_FAILED);
    requestUpdate();
    return;
  }
  if (!Storage.exists(kBackup)) {
    resultText = tr(STR_SETTINGS_BACKUP_MISSING);
    requestUpdate();
    return;
  }
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_SETTINGS_BACKUP_IMPORT),
                                                                tr(STR_SETTINGS_BACKUP_IMPORT_CONFIRM)),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled)
                             resultText =
                                 restoreBackup() ? tr(STR_SETTINGS_BACKUP_RESTORED) : tr(STR_SETTINGS_BACKUP_FAILED);
                           requestUpdate();
                         });
}

void SettingsBackupActivity::loop() {
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

void SettingsBackupActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int height = renderer.getScreenHeight();
  const auto layout = UiLayout::from(renderer);
  const int centerOffset = layout.content.x + layout.content.width / 2 - renderer.getScreenWidth() / 2;
  const int bottomHints = layout.landscape ? 0 : metrics.buttonHintsHeight + metrics.verticalSpacing;
  GUI.drawHeader(renderer, Rect{layout.content.x, metrics.topPadding, layout.content.width, metrics.headerHeight},
                 tr(STR_SETTINGS_BACKUP));
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  GUI.drawList(renderer, Rect{layout.content.x, top, layout.content.width, height - top - bottomHints}, kItemCount,
               selectedIndex, [](int index) {
                 return std::string(index == 0 ? tr(STR_SETTINGS_BACKUP_EXPORT) : tr(STR_SETTINGS_BACKUP_IMPORT));
               });
  if (!resultText.empty()) {
    const int resultY = height - (layout.landscape ? metrics.verticalSpacing * 2
                                                   : metrics.buttonHintsHeight + metrics.verticalSpacing * 2);
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, resultY, resultText.c_str(), true, centerOffset);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
