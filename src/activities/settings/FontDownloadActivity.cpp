#include "FontDownloadActivity.h"

#include <ArduinoJson.h>
#include <FontManager.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdint>

#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/TlsHeapReclaim.h"
#include "util/ScreenshotUtil.h"

namespace {
struct PendingFontFile {
  std::string finalPath;
  std::string tempPath;
  std::string backupPath;
  bool backupCreated = false;
  bool installed = false;
};

constexpr int FONT_DOWNLOAD_MAX_RETRIES = 5;
constexpr unsigned long FONT_DOWNLOAD_STREAM_IDLE_TIMEOUT_MS = 90000;
// E-paper redraws take roughly 670 ms, so redrawing for every 512-byte network
// write can keep the render task busy for the entire download. Two progress
// updates per file still provide useful feedback without throttling reception.
constexpr int FONT_DOWNLOAD_PROGRESS_STEP_PERCENT = 50;

bool isRetryableFontDownloadFailure(const HttpDownloader::DownloadError result) {
  if (result != HttpDownloader::HTTP_ERROR) return false;

  const int httpCode = HttpDownloader::lastHttpCode;
  return httpCode <= 0 || httpCode == 408 || httpCode == 429 || httpCode >= 500;
}

void removePendingTemps(const std::vector<PendingFontFile>& files) {
  for (const auto& file : files) Storage.remove(file.tempPath.c_str());
}

void restoreFontBackups(std::vector<PendingFontFile>& files) {
  for (auto it = files.rbegin(); it != files.rend(); ++it) {
    if (it->installed) Storage.remove(it->finalPath.c_str());
    if (it->backupCreated && Storage.exists(it->backupPath.c_str())) {
      if (!Storage.rename(it->backupPath.c_str(), it->finalPath.c_str())) {
        LOG_ERR("FONT", "Failed to restore font backup: %s", it->finalPath.c_str());
      }
    }
  }
  removePendingTemps(files);
}
}  // namespace

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FontDownload", renderer, mappedInput), fontInstaller_(sdFontSystem.registry()) {}

// --- Lifecycle ---

void FontDownloadActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FontDownloadActivity::onExit() {
  Activity::onExit();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);

  // Reload font caches that were freed for TLS memory
  FontManager::getInstance().loadSettings();
}

void FontDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state_ = LOADING_MANIFEST;
  }
  requestUpdateAndWait();

  // Settings screens use built-in UI fonts.  Release any reader font family
  // before the first TLS allocation so its data cannot fragment X3's heap.
  // The selected family is reloaded automatically when a book is opened.
  sdFontSystem.releaseLoadedFamily(renderer);

  reclaimHeapForTls(renderer, "FONT");

  if (!fetchAndParseManifest()) {
    {
      RenderLock lock(*this);
      state_ = ERROR;
    }
    return;
  }

  {
    RenderLock lock(*this);
    state_ = FAMILY_LIST;
    selectedIndex_ = 0;
  }
}

// --- Manifest fetching ---

bool FontDownloadActivity::fetchAndParseManifest() {
  // Download manifest to SD card temp file, then parse from file.
  // This avoids holding both TLS buffers and manifest data in RAM.
  static constexpr const char* MANIFEST_TMP = "/.fonts_manifest.tmp";

  const size_t heapBefore = ESP.getFreeHeap();
  auto result = HttpDownloader::downloadToFile(FONT_MANIFEST_URL, MANIFEST_TMP, nullptr);
  if (result != HttpDownloader::OK) {
    LOG_ERR("FONT", "Manifest fetch failed (err=%d, http=%d, heap=%zu)", result, HttpDownloader::lastHttpCode,
            heapBefore);
    char buf[80];
    snprintf(buf, sizeof(buf), "err=%d http=%d heap=%zuKB", static_cast<int>(result), HttpDownloader::lastHttpCode,
             heapBefore / 1024);
    errorMessage_ = buf;
    Storage.remove(MANIFEST_TMP);
    return false;
  }

  // TLS connection closed — buffers freed. Parse JSON from file.
  FsFile manifestFile;
  if (!Storage.openFileForRead("FONT", MANIFEST_TMP, manifestFile)) {
    LOG_ERR("FONT", "Failed to open temp manifest");
    Storage.remove(MANIFEST_TMP);
    errorMessage_ = "Failed to read manifest";
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, manifestFile);
  manifestFile.close();
  Storage.remove(MANIFEST_TMP);

  if (err) {
    LOG_ERR("FONT", "Manifest parse failed: %s (heap=%u max=%u)", err.c_str(), ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    errorMessage_ = std::string("Manifest parse failed: ") + err.c_str();
    return false;
  }

  int version = doc["version"] | 0;
  if (version != 1) {
    LOG_ERR("FONT", "Unsupported manifest version: %d", version);
    errorMessage_ = "Unsupported manifest version";
    return false;
  }

  snprintf(baseUrl_, sizeof(baseUrl_), "%s", doc["baseUrl"] | "");
  families_.clear();
  allFiles_.clear();

  JsonArray familiesArr = doc["families"].as<JsonArray>();
  if (familiesArr.isNull()) {
    LOG_ERR("FONT", "Manifest has no families array");
    errorMessage_ = "Invalid font manifest";
    return false;
  }
  families_.reserve(familiesArr.size());

  size_t fileCountTotal = 0;
  for (JsonObject fObj : familiesArr) fileCountTotal += fObj["files"].as<JsonArray>().size();
  allFiles_.reserve(fileCountTotal);

  // Pick up files copied to the SD card before opening this screen.  Do this
  // once, rather than once per manifest family, because discovery itself is an
  // SD-card directory scan.
  fontInstaller_.refreshRegistry();

  for (JsonObject fObj : familiesArr) {
    ManifestFamily family;
    snprintf(family.name, sizeof(family.name), "%s", fObj["name"] | "");
    snprintf(family.description, sizeof(family.description), "%s", fObj["description"] | "");

    family.fileStart = static_cast<uint16_t>(allFiles_.size());
    family.fileCount = 0;
    family.totalSize = 0;
    for (JsonObject fileObj : fObj["files"].as<JsonArray>()) {
      if (family.fileCount == UINT8_MAX) {
        LOG_ERR("FONT", "Too many files in family %s", family.name);
        break;
      }
      ManifestFile file;
      snprintf(file.name, sizeof(file.name), "%s", fileObj["name"] | "");
      file.size = fileObj["size"] | 0u;
      snprintf(file.sha256, sizeof(file.sha256), "%s", fileObj["sha256"] | "");
      family.totalSize += file.size;
      allFiles_.push_back(file);
      family.fileCount++;
    }

    // Keep the actual family path:
    // older ZIPs used /fonts or /.crosspoint/fonts, while downloads now use
    // /.fonts.  Comparing only against /.fonts makes every legacy install look
    // like an update forever.
    const auto* installedFamily = sdFontSystem.registry().findFamily(family.name);
    family.installed = installedFamily != nullptr;

    // Detect updates from file sizes only.  Hashing every installed CJK font
    // here would read hundreds of megabytes from the SD card before the list
    // can be shown (especially noticeable on X3).  The actual download path
    // still validates the SHA-256 before replacing any installed file.
    if (family.installed) {
      for (uint8_t i = 0; i < family.fileCount; i++) {
        const auto& file = allFiles_[family.fileStart + i];
        char path[160];
        snprintf(path, sizeof(path), "%s/%s", installedFamily->path.c_str(), file.name);
        FsFile f;
        if (Storage.openFileForRead("FONT", path, f)) {
          size_t actual = f.fileSize();
          f.close();
          if (actual != file.size) {
            family.hasUpdate = true;
            break;
          }
        } else {
          // File missing on disk but family dir exists — treat as update
          family.hasUpdate = true;
          break;
        }
      }
    }

    families_.push_back(family);
  }

  LOG_DBG("FONT", "Manifest loaded: %zu families", families_.size());
  return true;
}

// --- Download ---

void FontDownloadActivity::downloadAll() {
  for (size_t i = 0; i < families_.size(); i++) {
    if (families_[i].installed && !families_[i].hasUpdate) continue;
    if (!downloadFamily(families_[i])) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

size_t FontDownloadActivity::totalUninstalledSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (!f.installed || f.hasUpdate) total += f.totalSize;
  }
  return total;
}

bool FontDownloadActivity::downloadFamily(ManifestFamily& family) {
  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingFamilyIndex_ = static_cast<int>(&family - families_.data());
    currentFileIndex_ = 0;
    currentFileTotal_ = family.fileCount;
    fileProgress_ = 0;
    fileTotal_ = 0;
    screenshotHeldDuringDownload_ = false;
  }
  requestUpdateAndWait();

  if (!fontInstaller_.ensureFamilyDir(family.name)) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to create font directory";
    return false;
  }

  std::vector<PendingFontFile> pending;
  pending.reserve(family.fileCount);
  for (uint8_t i = 0; i < family.fileCount; i++) {
    const auto& file = allFiles_[family.fileStart + i];
    char finalPath[128];
    FontInstaller::buildFontPath(family.name, file.name, finalPath, sizeof(finalPath));
    PendingFontFile entry;
    entry.finalPath = finalPath;
    entry.tempPath = entry.finalPath + ".update.tmp";
    entry.backupPath = entry.finalPath + ".update.bak";

    // Recover a file interrupted between moving the old version aside and
    // promoting the verified temporary file. A completed replacement leaves
    // both final and backup; the final is authoritative in that case.
    if (Storage.exists(entry.backupPath.c_str())) {
      if (Storage.exists(entry.finalPath.c_str())) {
        Storage.remove(entry.backupPath.c_str());
      } else if (!Storage.rename(entry.backupPath.c_str(), entry.finalPath.c_str())) {
        LOG_ERR("FONT", "Failed to recover interrupted update: %s", entry.finalPath.c_str());
      }
    }
    // Keep partial files from a cancelled or interrupted transfer. Each file
    // is resumed with HTTP Range on the next attempt and verified before use.
    pending.push_back(std::move(entry));
  }

  for (uint8_t i = 0; i < family.fileCount; i++) {
    const auto& file = allFiles_[family.fileStart + i];

    size_t resumeFrom = 0;
    if (Storage.exists(pending[i].tempPath.c_str())) {
      FsFile partialFile;
      if (Storage.openFileForRead("FONT", pending[i].tempPath, partialFile)) {
        resumeFrom = partialFile.fileSize();
        partialFile.close();
      }
      if (resumeFrom > file.size) {
        Storage.remove(pending[i].tempPath.c_str());
        resumeFrom = 0;
      } else if (resumeFrom == file.size &&
                 (!fontInstaller_.validateCpfontFile(pending[i].tempPath.c_str()) ||
                  !fontInstaller_.verifySha256File(pending[i].tempPath.c_str(), file.sha256))) {
        Storage.remove(pending[i].tempPath.c_str());
        resumeFrom = 0;
      }
    }

    {
      RenderLock lock(*this);
      currentFileIndex_ = i;
      fileProgress_ = resumeFrom;
      fileTotal_ = file.size;
    }
    requestUpdateAndWait();

    // A prior run may already have downloaded and verified this whole file.
    if (resumeFrom == file.size) continue;

    char url[192];
    snprintf(url, sizeof(url), "%s%s", baseUrl_, file.name);

    HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
    for (int attempt = 0; attempt < FONT_DOWNLOAD_MAX_RETRIES; ++attempt) {
      if (attempt > 0) {
        LOG_DBG("FONT", "Retrying download %d/%d: %s", attempt + 1, FONT_DOWNLOAD_MAX_RETRIES, file.name);
        delay(static_cast<unsigned long>(attempt) * 1000);
      }

      // The progress screen can refill font caches between files, so reclaim
      // again immediately before every TLS handshake.
      reclaimHeapForTls(renderer, "FONT");
      if (Storage.exists(pending[i].tempPath.c_str())) {
        FsFile partialFile;
        if (Storage.openFileForRead("FONT", pending[i].tempPath, partialFile)) {
          resumeFrom = partialFile.fileSize();
          partialFile.close();
        }
        if (resumeFrom > file.size) {
          Storage.remove(pending[i].tempPath.c_str());
          resumeFrom = 0;
        }
      }
      int lastDisplayedPercent =
          file.size > 0 ? static_cast<int>((static_cast<uint64_t>(resumeFrom) * 100) / file.size) : 0;
      result = HttpDownloader::downloadToFile(
          url, pending[i].tempPath,
          [this, &lastDisplayedPercent](size_t downloaded, size_t total) {
            const int percent = total > 0 ? static_cast<int>((static_cast<uint64_t>(downloaded) * 100) / total) : 0;
            if (downloaded < total && percent < lastDisplayedPercent + FONT_DOWNLOAD_PROGRESS_STEP_PERCENT) return;

            fileProgress_ = downloaded;
            fileTotal_ = total;
            lastDisplayedPercent = percent;
            requestUpdate(true);
          },
          30000, "", "", resumeFrom,
          [this] {
            mappedInput.update();

            // Font downloads run synchronously, so the main-loop screenshot
            // shortcut is not reached. Handle its raw POWER + DOWN chord here
            // and consume it before the activity sees any cancellation input.
            const bool screenshotPressed = gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN);
            if (screenshotPressed) {
              if (!screenshotHeldDuringDownload_) {
                screenshotHeldDuringDownload_ = true;
                ScreenshotUtil::takeScreenshot(renderer);
              }
              return false;
            }
            screenshotHeldDuringDownload_ = false;
            return mappedInput.wasPressed(MappedInputManager::Button::Back);
          },
          /*preservePartialOnError=*/true,
          FONT_DOWNLOAD_STREAM_IDLE_TIMEOUT_MS);
      if (result == HttpDownloader::OK || !isRetryableFontDownloadFailure(result)) break;

      LOG_ERR("FONT", "Download attempt %d/%d failed: %s (err=%d http=%d)", attempt + 1, FONT_DOWNLOAD_MAX_RETRIES,
              file.name, static_cast<int>(result), HttpDownloader::lastHttpCode);
    }

    if (result == HttpDownloader::ABORTED) {
      LOG_INF("FONT", "Download cancelled: %s", file.name);
      RenderLock lock(*this);
      state_ = FAMILY_LIST;
      return false;
    }

    if (result != HttpDownloader::OK) {
      LOG_ERR("FONT", "Download failed: %s (%d)", file.name, result);
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = std::string("Download failed: ") + file.name;
      return false;
    }

    if (!fontInstaller_.validateCpfontFile(pending[i].tempPath.c_str()) ||
        !fontInstaller_.verifySha256File(pending[i].tempPath.c_str(), file.sha256)) {
      LOG_ERR("FONT", "Invalid or corrupt .cpfont: %s", pending[i].tempPath.c_str());
      removePendingTemps(pending);
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = std::string("Invalid font file: ") + file.name;
      return false;
    }
  }

  // Only replace installed files after every member of the family is complete
  // and verified. If a rename fails, restore the previous versions.
  for (auto& file : pending) {
    Storage.remove(file.backupPath.c_str());
    if (Storage.exists(file.finalPath.c_str())) {
      if (!Storage.rename(file.finalPath.c_str(), file.backupPath.c_str())) {
        LOG_ERR("FONT", "Failed to back up installed font: %s", file.finalPath.c_str());
        restoreFontBackups(pending);
        RenderLock lock(*this);
        state_ = ERROR;
        errorMessage_ = "Failed to install font update";
        return false;
      }
      file.backupCreated = true;
    }
    if (!Storage.rename(file.tempPath.c_str(), file.finalPath.c_str())) {
      LOG_ERR("FONT", "Failed to install verified font: %s", file.finalPath.c_str());
      restoreFontBackups(pending);
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = "Failed to install font update";
      return false;
    }
    file.installed = true;
  }
  for (const auto& file : pending) Storage.remove(file.backupPath.c_str());

  fontInstaller_.refreshRegistry();
  family.installed = true;
  family.hasUpdate = false;

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
  return true;
}

// --- Input handling ---

void FontDownloadActivity::loop() {
  if (state_ == FAMILY_LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    buttonNavigator_.onNextRelease([this] {
      if (selectedIndex_ < listItemCount() - 1) {
        selectedIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onPreviousRelease([this] {
      if (selectedIndex_ > 0) {
        selectedIndex_--;
        requestUpdate();
      }
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!families_.empty()) {
        RenderLock lock(*this);
        state_ = CONFIRM_DOWNLOAD;
      }
    }
  } else if (state_ == CONFIRM_DOWNLOAD) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      RenderLock lock(*this);
      state_ = FAMILY_LIST;
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (isDownloadAllSelected()) {
        downloadAll();
      } else {
        downloadFamily(families_[familyIndexFromList(selectedIndex_)]);
      }
      requestUpdate();
    }
  } else if (state_ == COMPLETE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      RenderLock lock(*this);
      state_ = FAMILY_LIST;
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      RenderLock lock(*this);
      state_ = FAMILY_LIST;
    }
  }
}

// --- Rendering ---

std::string FontDownloadActivity::formatSize(size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

void FontDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_DOWNLOAD));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  } else if (state_ == FAMILY_LIST) {
    if (families_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_FONTS_AVAILABLE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer,
          Rect{0, contentTop, pageWidth, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
          listItemCount(), selectedIndex_,
          [this](int index) -> std::string {
            if (index == 0) {
              return std::string(tr(STR_DOWNLOAD_ALL)) + " (" + formatSize(totalUninstalledSize()) + ")";
            }
            return families_[familyIndexFromList(index)].name;
          },
          nullptr, nullptr,
          [this](int index) -> std::string {
            if (index == 0) return "";
            const auto& f = families_[familyIndexFromList(index)];
            if (f.hasUpdate) return tr(STR_UPDATE_AVAILABLE);
            if (f.installed) return tr(STR_INSTALLED);
            return f.description;
          },
          true,
          [this](int index) -> bool {
            if (index == 0) return false;
            const auto& f = families_[familyIndexFromList(index)];
            // Dim installed fonts, but not those with updates available
            return f.installed && !f.hasUpdate;
          });

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state_ == CONFIRM_DOWNLOAD) {
    int y = contentTop;

    if (isDownloadAllSelected()) {
      std::string confirmText = std::string(tr(STR_DOWNLOAD_ALL)) + "?";
      renderer.drawCenteredText(UI_10_FONT_ID, y, confirmText.c_str());
      y += lineHeight + metrics.verticalSpacing;

      size_t totalFiles = 0;
      for (const auto& f : families_) {
        if (!f.installed) totalFiles += f.fileCount;
      }
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y,
                        (std::string(tr(STR_FILES_LABEL)) + std::to_string(totalFiles)).c_str());
      y += lineHeight + metrics.verticalSpacing;
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y,
                        (std::string(tr(STR_SIZE_LABEL)) + formatSize(totalUninstalledSize())).c_str());
    } else {
      const auto& family = families_[familyIndexFromList(selectedIndex_)];
      std::string confirmText = (family.installed ? std::string(tr(STR_REDOWNLOAD)) : std::string(tr(STR_DOWNLOAD))) +
                                " " + std::string(family.name) + "?";
      renderer.drawCenteredText(UI_10_FONT_ID, y, confirmText.c_str());
      y += lineHeight + metrics.verticalSpacing;
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y,
                        (std::string(tr(STR_FILES_LABEL)) + std::to_string(family.fileCount)).c_str());
      y += lineHeight + metrics.verticalSpacing;
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y,
                        (std::string(tr(STR_SIZE_LABEL)) + formatSize(family.totalSize)).c_str());
    }

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == DOWNLOADING) {
    const auto& family = families_[downloadingFamilyIndex_];

    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + std::string(family.name) + " (" +
                             std::to_string(currentFileIndex_ + 1) + "/" + std::to_string(currentFileTotal_) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());

    float progress = 0;
    if (fileTotal_ > 0) {
      progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    }

    int barY = centerY + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    int percentY = barY + metrics.progressBarHeight + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, percentY,
                              (std::to_string(static_cast<int>(progress * 100)) + "%").c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_FONT_INSTALLED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_FONT_INSTALL_FAILED), true,
                              EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
