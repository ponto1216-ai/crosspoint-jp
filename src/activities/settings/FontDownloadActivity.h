#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include "FontInstaller.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

#ifndef FONT_MANIFEST_URL
#define FONT_MANIFEST_URL "https://ponto1216-ai.github.io/crosspoint-jp/fonts/fonts.json"
#endif

class FontDownloadActivity : public Activity {
 public:
  explicit FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state_ == LOADING_MANIFEST || state_ == DOWNLOADING; }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_MANIFEST,
    FAMILY_LIST,
    CONFIRM_DOWNLOAD,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  // Keep manifest records in fixed-size storage.  The ESP32-C3 has enough
  // total heap for this manifest, but hundreds of short std::string
  // allocations fragment the largest free block and can starve the next TLS
  // handshake used for a font download.
  struct ManifestFile {
    char name[40] = {0};
    uint32_t size = 0;
    char sha256[65] = {0};
  };

  struct ManifestFamily {
    char name[32] = {0};
    char description[80] = {0};
    uint16_t fileStart = 0;
    uint8_t fileCount = 0;
    uint32_t totalSize = 0;
    bool installed = false;
    bool hasUpdate = false;
  };

  State state_ = WIFI_SELECTION;
  FontInstaller fontInstaller_;
  ButtonNavigator buttonNavigator_;

  // Manifest data
  char baseUrl_[96] = {0};
  std::vector<ManifestFamily> families_;
  std::vector<ManifestFile> allFiles_;
  int selectedIndex_ = 0;

  // Download progress
  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadingFamilyIndex_ = 0;
  bool screenshotHeldDuringDownload_ = false;
  std::string errorMessage_;

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifest();
  bool downloadFamily(ManifestFamily& family);
  void downloadAll();
  bool isDownloadAllSelected() const { return selectedIndex_ == 0 && !families_.empty(); }
  int familyIndexFromList(int listIndex) const { return listIndex - 1; }
  int listItemCount() const { return families_.empty() ? 0 : static_cast<int>(families_.size()) + 1; }
  size_t totalUninstalledSize() const;
  static std::string formatSize(size_t bytes);
};
