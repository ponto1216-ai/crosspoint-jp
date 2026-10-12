#include "DiagnosticsActivity.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalRTC.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <ctime>
#include <string_view>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"
#if FREEINK_DEVICE_X4CLASSIC && !defined(SIMULATOR)
#include "util/X4cDiagnostics.h"
#endif

namespace {

constexpr const char* kDiagnosticsDirectory = "/.crosspoint/diagnostics";

const char* displayControllerName() {
  switch (BoardConfig::ACTIVE.displayController) {
    case BoardConfig::DisplayController::SSD1677:
      return "SSD1677";
    case BoardConfig::DisplayController::UC8253:
      return "UC8253";
    case BoardConfig::DisplayController::UC8279:
      return "UC8279";
    case BoardConfig::DisplayController::UC8179:
      return "UC8179";
    default:
      return "unknown";
  }
}

const char* deviceName() {
  switch (BoardConfig::ACTIVE.board) {
    case BoardConfig::Board::XteinkX3:
    case BoardConfig::Board::XteinkX3Uc8279:
      return "X3";
    case BoardConfig::Board::XteinkX4:
      return "X4";
    case BoardConfig::Board::XteinkX4Classic:
      return "X4 Classic";
    case BoardConfig::Board::XteinkX4Pro:
      return "X4 Pro";
    default:
      return BoardConfig::ACTIVE.name;
  }
}

std::string deviceDescription() { return std::string(deviceName()) + " (" + displayControllerName() + ")"; }

const char* inputStyleName() {
#ifdef SIMULATOR
  return "simulated_buttons";
#else
  switch (BoardConfig::ACTIVE.inputStyle) {
    case BoardConfig::InputStyle::XteinkAdcLadder:
      return "xteink_adc_ladder";
    case BoardConfig::InputStyle::DigitalButtons:
      return "digital_buttons";
    default:
      return "other";
  }
#endif
}

const char* sdTransportName() {
#if FREEINK_SD_SDMMC
  return "sdmmc";
#else
  return "spi";
#endif
}

std::vector<std::string> splitLogLines(const std::string& logs) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < logs.size()) {
    const size_t end = logs.find('\n', start);
    const size_t length = end == std::string::npos ? logs.size() - start : end - start;
    if (length > 0) lines.push_back(logs.substr(start, length));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return lines;
}

std::string makeReportPath() {
  const time_t now = time(nullptr);
  if (now >= 1704067200) {
    struct tm timeInfo{};
    localtime_r(&now, &timeInfo);
    char filename[48];
    snprintf(filename, sizeof(filename), "report_%04d%02d%02d_%02d%02d%02d.txt", timeInfo.tm_year + 1900,
             timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
    return std::string(kDiagnosticsDirectory) + "/" + filename;
  }
  return std::string(kDiagnosticsDirectory) + "/report_boot_" + std::to_string(millis()) + ".txt";
}

bool isReadingCacheDirectory(const std::string_view name) {
  return name.rfind("epub_", 0) == 0 || name.rfind("xtc_", 0) == 0 || name.rfind("txt_", 0) == 0;
}

uint64_t directorySize(const std::string& path, bool& complete) {
  auto directory = Storage.open(path.c_str());
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    complete = false;
    return 0;
  }

  uint64_t total = 0;
  char name[128];
  for (auto entry = directory.openNextFile(); entry; entry = directory.openNextFile()) {
    const bool isDirectory = entry.isDirectory();
    entry.getName(name, sizeof(name));
    const std::string childPath = path + "/" + name;
    if (isDirectory) {
      entry.close();
      total += directorySize(childPath, complete);
    } else {
      total += entry.size();
      entry.close();
    }
    delay(0);
  }
  directory.close();
  return total;
}

struct ReadingCacheUsage {
  int directoryCount = 0;
  uint64_t bytes = 0;
  bool complete = true;
};

ReadingCacheUsage collectReadingCacheUsage() {
  ReadingCacheUsage usage;
  auto root = Storage.open("/.crosspoint");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    usage.complete = false;
    return usage;
  }

  char name[128];
  for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    const bool isDirectory = entry.isDirectory();
    entry.getName(name, sizeof(name));
    entry.close();
    if (!isDirectory || !isReadingCacheDirectory(name)) continue;
    ++usage.directoryCount;
    usage.bytes += directorySize(std::string("/.crosspoint/") + name, usage.complete);
    delay(0);
  }
  root.close();
  return usage;
}

std::string formatBytes(const uint64_t bytes) {
  constexpr uint64_t kKiB = 1024;
  constexpr uint64_t kMiB = kKiB * 1024;
  constexpr uint64_t kGiB = kMiB * 1024;
  char buffer[32];
  if (bytes >= kGiB) {
    snprintf(buffer, sizeof(buffer), "%.1f GB", static_cast<double>(bytes) / kGiB);
  } else if (bytes >= kMiB) {
    snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / kMiB);
  } else if (bytes >= kKiB) {
    snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / kKiB);
  } else {
    snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
  }
  return buffer;
}

std::string extensionOf(const std::string& path) {
  const size_t slash = path.rfind('/');
  const size_t dot = path.rfind('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "unknown";
  return path.substr(dot + 1);
}

const char* cacheStatusName(const Epub::CacheGenerationStatus status) {
  switch (status) {
    case Epub::CacheGenerationStatus::NotGenerated:
      return "not_generated";
    case Epub::CacheGenerationStatus::Resumable:
      return "resumable";
    case Epub::CacheGenerationStatus::Complete:
      return "complete";
  }
  return "unknown";
}

}  // namespace

void DiagnosticsActivity::onEnter() {
  Activity::onEnter();
  collectSnapshot();
  entrySnapshotDurationMs = snapshotDurationMs;
  requestUpdate();
}

void DiagnosticsActivity::collectSnapshot(const bool includeStorageUsage) {
  const uint32_t startedAt = millis();
  {
    RenderLock lock;
    gaugeDiagnostics = powerManager.readGaugeDiagnostics();
  }
  sdReady = Storage.ready();
  freeHeap = ESP.getFreeHeap();
  maxAllocHeap = ESP.getMaxAllocHeap();
  minFreeHeap = ESP.getMinFreeHeap();
  // FAT free-space and recursive cache scans can take seconds on a large card.
  // Keep entry responsive; collect these only when saving a complete report.
  if (includeStorageUsage) {
    const uint32_t usageStartedAt = millis();
#ifdef SIMULATOR
    sdTotalBytes = 0;
    sdUsedBytes = 0;
#else
    sdTotalBytes = sdReady ? Storage.totalBytes() : 0;
    sdUsedBytes = sdReady ? Storage.usedBytes() : 0;
#endif
    if (sdUsedBytes > sdTotalBytes) sdUsedBytes = 0;
    sdUsageDurationMs = millis() - usageStartedAt;
    const uint32_t cacheStartedAt = millis();
    const auto cacheUsage = sdReady ? collectReadingCacheUsage() : ReadingCacheUsage{};
    cacheDirectoryCount = cacheUsage.directoryCount;
    readingCacheBytes = cacheUsage.bytes;
    readingCacheSizeComplete = cacheUsage.complete;
    cacheScanDurationMs = millis() - cacheStartedAt;
    storageUsageCollected = true;
  }
  hasActiveBook = static_cast<bool>(book);
  openBookType = "none";
  openBookSize = 0;
  const std::string bookPath = hasActiveBook ? book->getPath() : APP_STATE.openEpubPath;
  if (sdReady && !bookPath.empty()) {
    openBookType = extensionOf(bookPath);
    auto bookFile = Storage.open(bookPath.c_str());
    if (bookFile) {
      openBookSize = static_cast<uint32_t>(bookFile.size());
      bookFile.close();
    }
  }
  bookCacheStatus = hasActiveBook ? book->getCacheGenerationStatus() : Epub::CacheGenerationStatus::NotGenerated;
  bookFingerprint = 0;
  bookFingerprintAvailable = includeStorageUsage && hasActiveBook && book->getSourceFingerprint(&bookFingerprint);
  readerVertical = SETTINGS.writingMode == CrossPointSettings::WM_VERTICAL;
  const auto& direction = SETTINGS.getDirectionSettings(readerVertical);
  readerFont = direction.sdFontFamilyName[0] == '\0' ? "Noto Sans" : direction.sdFontFamilyName;
  readerLineSpacing = direction.lineSpacing;
  readerImageRendering = SETTINGS.imageRendering;
  readerBookStyle = SETTINGS.embeddedStyle;
  recentLogs = getLastLogs();
  recentLogLines = splitLogLines(recentLogs);
  snapshotDurationMs = millis() - startedAt;
}

bool DiagnosticsActivity::saveReport() {
#ifdef SIMULATOR
  // Report persistence depends on hardware-only storage and PSRAM metrics.
  // The screen and its state transitions remain available for UI testing.
  return false;
#else
  if (!sdReady || !Storage.ensureDirectoryExists(kDiagnosticsDirectory)) return false;

  savedReportPath = makeReportPath();
  auto file = Storage.open(savedReportPath.c_str(), O_WRITE | O_CREAT | O_TRUNC);
  if (!file) return false;

  file.printf("Yomuka diagnostics\n");
  file.printf("version=%s\n", CROSSPOINT_VERSION);
  file.printf("device=%s\n", deviceName());
  file.printf("display_controller=%s\n", displayControllerName());
#if FREEINK_DEVICE_X4CLASSIC
  file.printf("freeink_revision=%s\n", FREEINK_SDK_REVISION);
  file.printf("build_id=%s\n", CROSSPOINT_BUILD_ID);
  writeX4cHardwareDiagnostics(file);
#endif
  file.printf("input_style=%s\n", inputStyleName());
  file.printf("sd_transport=%s\n", sdTransportName());
  file.printf("rtc_available=%s\n", halRTC.isAvailable() ? "true" : "false");
  file.printf("battery_gauge_available=%s\n", gaugeDiagnostics.supported ? "true" : "false");
  if (gaugeDiagnostics.supported) {
    const auto& g = gaugeDiagnostics;
    file.printf("battery_gauge_valid=0x%02X\n", g.valid);
    file.printf(
        "battery_capacity_step=%u\nbattery_capacity_pending=%s\nbattery_capacity_changed=%s\n"
        "battery_capacity_verified=%s\nbattery_capacity_had_error=%s\n",
        g.capacityStep, g.capacityPending ? "true" : "false", g.capacityChanged ? "true" : "false",
        g.capacityVerified ? "true" : "false", g.capacityHadError ? "true" : "false");
    file.printf("battery_gauge_captured_ms=%lu\nbattery_gauge_read_ms=%lu\n", g.capturedMs, g.readMs);
    const char* names[] = {"soc_percent", "voltage_mV", "current_mA",     "remaining_mAh",
                           "full_mAh",    "design_mAh", "battery_status", "operation_status"};
    const uint16_t values[] = {g.soc,  g.voltage, g.current,       g.remaining,
                               g.full, g.design,  g.batteryStatus, g.operationStatus};
    for (unsigned i = 0; i < 8; ++i) {
      if (!(g.valid & (1u << i)))
        file.printf("battery_gauge_%s=unknown\n", names[i]);
      else if (i >= 6)
        file.printf("battery_gauge_%s=0x%04X\n", names[i], values[i]);
      else
        file.printf("battery_gauge_%s=%d\n", names[i],
                    i == 2 && values[i] >= 0x8000 ? static_cast<int>(values[i]) - 0x10000 : values[i]);
    }
    if (g.valid & 0x80) {
      file.printf("battery_gauge_security=%u\n", (g.operationStatus >> 1) & 3);
      file.printf("battery_gauge_cfgupdate=%s\n", (g.operationStatus & 0x0400) ? "true" : "false");
      file.printf("battery_gauge_initcomp=%s\n", (g.operationStatus & 0x0020) ? "true" : "false");
      file.printf("battery_gauge_edv2=%s\n", (g.operationStatus & 0x0008) ? "true" : "false");
    }
  }
  const auto& abortedSleep = powerManager.getAbortedSleepInfo();
  file.printf("previous_deep_sleep_aborted=%s\n", abortedSleep.aborted ? "true" : "false");
  if (abortedSleep.aborted) {
    file.printf("previous_deep_sleep_abort_cause=%d\n", abortedSleep.wakeupCause);
    file.printf("previous_deep_sleep_abort_wake_pin=%d\n", abortedSleep.wakePinLevel);
  }
  file.printf("psram_available=%s\n", psramFound() ? "true" : "false");
  file.printf("psram_total_bytes=%lu\n", static_cast<unsigned long>(ESP.getPsramSize()));
  file.printf("psram_free_bytes=%lu\n", static_cast<unsigned long>(ESP.getFreePsram()));
  file.printf("psram_max_alloc_bytes=%lu\n", static_cast<unsigned long>(ESP.getMaxAllocPsram()));
  file.printf("sd_ready=%s\n", sdReady ? "true" : "false");
  file.printf("sd_total_bytes=%llu\n", static_cast<unsigned long long>(sdTotalBytes));
  file.printf("sd_used_bytes=%llu\n", static_cast<unsigned long long>(sdUsedBytes));
  file.printf("sd_free_bytes=%llu\n", static_cast<unsigned long long>(sdTotalBytes - sdUsedBytes));
  file.printf("free_heap=%lu\n", static_cast<unsigned long>(freeHeap));
  file.printf("max_alloc_heap=%lu\n", static_cast<unsigned long>(maxAllocHeap));
  file.printf("min_free_heap=%lu\n", static_cast<unsigned long>(minFreeHeap));
  file.printf("reading_cache_directories=%d\n", cacheDirectoryCount);
  file.printf("reading_cache_bytes=%llu\n", static_cast<unsigned long long>(readingCacheBytes));
  file.printf("reading_cache_size_complete=%s\n", readingCacheSizeComplete ? "true" : "false");
  file.printf("open_book_type=%s\n", openBookType.c_str());
  file.printf("open_book_size=%lu\n", static_cast<unsigned long>(openBookSize));
  file.printf("active_book=%s\n", hasActiveBook ? "true" : "false");
  if (hasActiveBook) {
    file.printf("active_book_cache_status=%s\n", cacheStatusName(bookCacheStatus));
    if (bookFingerprintAvailable) {
      file.printf("active_book_source_fingerprint=%016llx\n", static_cast<unsigned long long>(bookFingerprint));
    }
    file.printf("active_book_spine_index=%d\n", bookSpineIndex);
    file.printf("active_book_page_index=%d\n", bookPageIndex);
    file.printf("active_book_page_count=%d\n", bookPageCount);
  }
  file.printf("reader_writing_mode=%s\n", readerVertical ? "vertical" : "horizontal_or_auto");
  file.printf("reader_font=%s\n", readerFont.c_str());
  file.printf("reader_line_spacing=%u\n", readerLineSpacing);
  file.printf("reader_book_style=%u\n", readerBookStyle);
  file.printf("reader_image_rendering=%u\n", readerImageRendering);
  file.printf("snapshot_duration_ms=%lu\n", static_cast<unsigned long>(snapshotDurationMs));
  file.printf("entry_snapshot_duration_ms=%lu\nsd_usage_duration_ms=%lu\ncache_scan_duration_ms=%lu\n",
              static_cast<unsigned long>(entrySnapshotDurationMs), static_cast<unsigned long>(sdUsageDurationMs),
              static_cast<unsigned long>(cacheScanDurationMs));
  file.printf("captured_millis=%lu\n", static_cast<unsigned long>(millis()));
  file.print("\nRecent logs:\n");
  file.print(recentLogs.c_str());
  file.close();
  LOG_INF("DIAG", "Saved diagnostics report: %s", savedReportPath.c_str());
  return true;
#endif
}

void DiagnosticsActivity::loop() {
  // Consume the release here so the reader does not treat it as a second Back.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  const bool saveRequested = mappedInput.wasPressed(MappedInputManager::Button::Confirm);
  if (saveRequested || mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    {
      RenderLock lock;
      checking = true;
      saveResult = SaveResult::None;
    }
    // Show the busy state before the SD scan blocks the main loop.
    requestUpdateAndWait();
    collectSnapshot(true);
    const bool saved = saveRequested && saveReport();
    {
      RenderLock lock;
      checking = false;
      if (saveRequested) saveResult = saved ? SaveResult::Saved : SaveResult::Failed;
    }
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    page = page == Page::Overview ? Page::Logs : page == Page::Logs ? Page::Details : Page::Overview;
    requestUpdate();
  }
}

void DiagnosticsActivity::renderOverview(const int x, int y, const int contentWidth, const int lineHeight) {
  const auto drawLine = [this, x, &y, lineHeight](const std::string& text) {
    renderer.drawText(UI_10_FONT_ID, x, y, text.c_str());
    y += lineHeight;
  };
  drawLine(std::string("Yomuka: ") + CROSSPOINT_VERSION);
  drawLine("Device: " + deviceDescription());
  drawLine(std::string("SD: ") + (sdReady ? "ready" : "unavailable"));
  if (sdReady) drawLine("SD free: " + (storageUsageCollected ? formatBytes(sdTotalBytes - sdUsedBytes) : "--"));
  drawLine("Heap: " + std::to_string(freeHeap));
  drawLine("Max alloc: " + std::to_string(maxAllocHeap));
  drawLine("Min free: " + std::to_string(minFreeHeap));
  drawLine("Cache: " + (storageUsageCollected
                            ? formatBytes(readingCacheBytes) + " (" + std::to_string(cacheDirectoryCount) + ")"
                            : "--"));
  if (hasActiveBook) {
    drawLine(std::string("Book cache: ") + cacheStatusName(bookCacheStatus));
    drawLine("Book page: " + std::to_string(bookPageIndex + 1) + "/" + std::to_string(bookPageCount));
  }
  drawLine("Recent logs: " + std::to_string(recentLogLines.size()));

  y += lineHeight / 2;
  if (saveResult == SaveResult::Saved) {
    renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_DIAGNOSTICS_REPORT_SAVED));
    y += lineHeight;
    for (const auto& wrapped : renderer.wrappedText(UI_10_FONT_ID, savedReportPath.c_str(), contentWidth, 2)) {
      renderer.drawText(UI_10_FONT_ID, x, y, wrapped.c_str());
      y += lineHeight;
    }
  } else if (saveResult == SaveResult::Failed) {
    renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_DIAGNOSTICS_REPORT_FAILED));
  }
}

void DiagnosticsActivity::renderLogs(const int x, int y, const int contentWidth, const int lineHeight) {
  if (recentLogLines.empty()) {
    renderer.drawText(UI_10_FONT_ID, x, y, "(no recent logs)");
    return;
  }

  const size_t firstLine = recentLogLines.size() > 4 ? recentLogLines.size() - 4 : 0;
  for (size_t i = firstLine; i < recentLogLines.size(); ++i) {
    for (const auto& wrapped : renderer.wrappedText(UI_10_FONT_ID, recentLogLines[i].c_str(), contentWidth, 2)) {
      renderer.drawText(UI_10_FONT_ID, x, y, wrapped.c_str());
      y += lineHeight;
    }
    y += 2;
  }
}

void DiagnosticsActivity::renderDetails(const int x, int y, const int contentWidth, const int lineHeight) {
  const auto drawLine = [this, x, &y, lineHeight](const std::string& text) {
    renderer.drawText(UI_10_FONT_ID, x, y, text.c_str());
    y += lineHeight;
  };

  drawLine("Snapshot: " + std::to_string(snapshotDurationMs) + " ms");
  drawLine("Cache scan: " + std::string(!storageUsageCollected     ? "--"
                                        : readingCacheSizeComplete ? "complete"
                                                                   : "incomplete"));
  drawLine("Logs captured: " + std::to_string(recentLogLines.size()));
  drawLine(std::string("Active book: ") + (hasActiveBook ? "yes" : "no"));
  if (!hasActiveBook) return;

  drawLine("Book type: " + openBookType + " (" + formatBytes(openBookSize) + ")");
  drawLine(std::string("Book cache: ") + cacheStatusName(bookCacheStatus));
  if (bookFingerprintAvailable) {
    char fingerprint[24];
    snprintf(fingerprint, sizeof(fingerprint), "%016llx", static_cast<unsigned long long>(bookFingerprint));
    drawLine(std::string("Source ID: ") + fingerprint);
  }
  drawLine("Spine/page: " + std::to_string(bookSpineIndex) + "/" + std::to_string(bookPageIndex + 1) + "/" +
           std::to_string(bookPageCount));
}

void DiagnosticsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto layout = UiLayout::from(renderer);
  const int x = layout.content.x + metrics.contentSidePadding;
  const int contentWidth = layout.content.width - 2 * metrics.contentSidePadding;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{layout.content.x, metrics.topPadding, layout.content.width, metrics.headerHeight},
                 hasActiveBook ? tr(STR_READER_DIAGNOSTICS) : tr(STR_DIAGNOSTICS));
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const char* pageTitle = page == Page::Overview ? tr(STR_DIAGNOSTICS_OVERVIEW)
                          : page == Page::Logs   ? tr(STR_DIAGNOSTICS_RECENT_LOGS)
                                                 : tr(STR_DIAGNOSTICS_DETAILS);
  renderer.drawText(UI_10_FONT_ID, x, y, pageTitle);
  y += lineHeight + metrics.verticalSpacing;

  if (checking) {
    GUI.drawPopup(renderer, tr(STR_DIAGNOSTICS_CHECKING));
    GUI.drawButtonHints(renderer, "", "", "", "");
    renderer.displayBuffer();
    return;
  }

  if (page == Page::Overview) {
    renderOverview(x, y, contentWidth, lineHeight);
  } else if (page == Page::Logs) {
    renderLogs(x, y, contentWidth, renderer.getLineHeight(UI_10_FONT_ID));
  } else {
    renderDetails(x, y, contentWidth, renderer.getLineHeight(UI_10_FONT_ID));
  }

  // Button-hint space is deliberately narrower than the page heading, so use
  // short action labels while retaining the descriptive titles above.
  const char* nextPageLabel = page == Page::Overview ? tr(STR_DIAGNOSTICS_LOG_BUTTON)
                              : page == Page::Logs   ? tr(STR_DIAGNOSTICS_DETAILS_BUTTON)
                                                     : tr(STR_DIAGNOSTICS_OVERVIEW);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SAVE), nextPageLabel, tr(STR_DIAGNOSTICS_RELOAD));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
