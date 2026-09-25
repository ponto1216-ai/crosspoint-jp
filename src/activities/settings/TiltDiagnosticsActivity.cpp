#include "TiltDiagnosticsActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>

#include <cstring>
#include <ctime>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"

namespace {
constexpr const char* kDiagnosticsDirectory = "/.crosspoint/diagnostics";

#ifdef SIMULATOR
struct SimulatorImuSample {
  float ax = 0.02f;
  float ay = -0.03f;
  float az = 0.99f;
  float gx = 8.0f;
  float gy = -3.0f;
  float gz = 1.0f;
};

struct TiltDiagnosticsSnapshot {
  bool available = true;
  bool awake = true;
  uint8_t i2cAddress = 0x6B;
  SimulatorImuSample sample{};
  float currentAxisDps = 8.0f;
  float displayAxisDps = 8.0f;
  const char* axisName = "GX";
  float triggerThresholdDps = 270.0f;
  float neutralThresholdDps = 50.0f;
  float rightPeakDps = 312.0f;
  float leftPeakDps = 298.0f;
  float idleNoiseDps = 7.0f;
  uint32_t readErrorCount = 0;
  uint16_t rightCrossings = 3;
  uint16_t leftCrossings = 3;
};

TiltDiagnosticsSnapshot simulatorDiagnostics;

void beginTiltDiagnostics() { simulatorDiagnostics = TiltDiagnosticsSnapshot{}; }
void endTiltDiagnostics() {}
void updateTiltDiagnostics() {}
void resetTiltDiagnostics() { simulatorDiagnostics = TiltDiagnosticsSnapshot{}; }
TiltDiagnosticsSnapshot getTiltDiagnostics() { return simulatorDiagnostics; }
float tiltTriggerThreshold() { return simulatorDiagnostics.triggerThresholdDps; }
#else
using TiltDiagnosticsSnapshot = HalTiltSensor::Diagnostics;

void beginTiltDiagnostics() { halTiltSensor.beginDiagnostics(SETTINGS.orientation); }
void endTiltDiagnostics() { halTiltSensor.endDiagnostics(); }
void updateTiltDiagnostics() { halTiltSensor.updateDiagnostics(SETTINGS.orientation); }
void resetTiltDiagnostics() { halTiltSensor.resetDiagnostics(); }
TiltDiagnosticsSnapshot getTiltDiagnostics() { return halTiltSensor.getDiagnostics(SETTINGS.orientation); }
float tiltTriggerThreshold() { return HalTiltSensor::triggerThreshold(); }
#endif

std::string makeReportPath() {
  const time_t now = time(nullptr);
  if (now >= 1704067200) {
    struct tm timeInfo{};
    localtime_r(&now, &timeInfo);
    char filename[56];
    snprintf(filename, sizeof(filename), "imu_report_%04d%02d%02d_%02d%02d%02d.txt", timeInfo.tm_year + 1900,
             timeInfo.tm_mon + 1, timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
    return std::string(kDiagnosticsDirectory) + "/" + filename;
  }
  return std::string(kDiagnosticsDirectory) + "/imu_report_boot_" + std::to_string(millis()) + ".txt";
}
}  // namespace

void TiltDiagnosticsActivity::onEnter() {
  Activity::onEnter();
  beginTiltDiagnostics();
  lastRenderRequestMs = millis();
  requestUpdate();
}

void TiltDiagnosticsActivity::onExit() {
  endTiltDiagnostics();
  Activity::onExit();
}

const char* TiltDiagnosticsActivity::recommendation() const {
  const auto d = getTiltDiagnostics();
  if (d.rightPeakDps >= tiltTriggerThreshold() && d.leftPeakDps >= tiltTriggerThreshold()) {
    return tr(STR_TILT_RECOMMEND_NORMAL);
  }
  if (d.rightPeakDps >= d.neutralThresholdDps && d.leftPeakDps >= d.neutralThresholdDps) {
    return tr(STR_TILT_RECOMMEND_HIGH);
  }
  return tr(STR_TILT_RECOMMEND_RETRY);
}

bool TiltDiagnosticsActivity::saveReport() {
#ifdef SIMULATOR
  return false;
#else
  if (!Storage.ready() || !Storage.ensureDirectoryExists(kDiagnosticsDirectory)) return false;
  savedReportPath = makeReportPath();
  auto file = Storage.open(savedReportPath.c_str(), O_WRITE | O_CREAT | O_TRUNC);
  if (!file) return false;
  const auto d = getTiltDiagnostics();
  file.printf("Yomuka IMU diagnostics\n");
  file.printf("version=%s\n", CROSSPOINT_VERSION);
  file.printf("device=X3\n");
  file.printf("imu=QMI8658\n");
  file.printf("i2c_address=0x%02X\n", d.i2cAddress);
  file.printf("freeink_revision=%s\n", FREEINK_SDK_REVISION);
  file.printf("state=%s\n", d.awake ? "awake" : "sleep");
  file.printf("orientation=%u\n", SETTINGS.orientation);
  file.printf("axis=%s\n", d.axisName);
  file.printf("page_turn_basis=CrossPoint_1.6.5rc\n");
  file.printf("trigger_threshold_dps=%.1f\n", d.triggerThresholdDps);
  file.printf("neutral_threshold_dps=%.1f\n", d.neutralThresholdDps);
  file.printf("accel_g=%.4f,%.4f,%.4f\n", d.sample.ax, d.sample.ay, d.sample.az);
  file.printf("gyro_dps=%.2f,%.2f,%.2f\n", d.sample.gx, d.sample.gy, d.sample.gz);
  file.printf("current_axis_dps=%.2f\n", d.currentAxisDps);
  file.printf("display_axis_dps=%.2f\n", d.displayAxisDps);
  file.printf("right_peak_dps=%.2f\n", d.rightPeakDps);
  file.printf("left_peak_dps=%.2f\n", d.leftPeakDps);
  file.printf("idle_noise_dps=%.2f\n", d.idleNoiseDps);
  file.printf("right_crossings=%u\n", d.rightCrossings);
  file.printf("left_crossings=%u\n", d.leftCrossings);
  file.printf("i2c_read_errors=%lu\n", static_cast<unsigned long>(d.readErrorCount));
  file.close();
  LOG_INF("IMUDIAG", "Saved IMU report: %s", savedReportPath.c_str());
  return true;
#endif
}

void TiltDiagnosticsActivity::loop() {
  updateTiltDiagnostics();
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    saveResult = saveReport() ? SaveResult::Saved : SaveResult::Failed;
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    resetTiltDiagnostics();
    saveResult = SaveResult::None;
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    page = page == Page::Test ? Page::Details : Page::Test;
    requestUpdate();
    return;
  }
  if ((millis() - lastRenderRequestMs) >= 2000) {
    lastRenderRequestMs = millis();
    requestUpdate();
  }
}

void TiltDiagnosticsActivity::drawWrapped(const char* text, const int x, int& y, const int width, const int lineHeight,
                                          const int maxLines) {
  const char* segment = text;
  int linesDrawn = 0;
  while (*segment != '\0' && linesDrawn < maxLines) {
    const char* newline = strchr(segment, '\n');
    const std::string paragraph(segment, newline == nullptr ? strlen(segment) : static_cast<size_t>(newline - segment));
    for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, paragraph.c_str(), width, maxLines - linesDrawn)) {
      renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
      y += lineHeight;
      if (++linesDrawn >= maxLines) return;
    }
    if (newline == nullptr) break;
    segment = newline + 1;
  }
}

void TiltDiagnosticsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto layout = UiLayout::from(renderer);
  const int x = layout.content.x + metrics.contentSidePadding;
  const int contentWidth = layout.content.width - 2 * metrics.contentSidePadding;
  const int centerOffset = layout.content.x + layout.content.width / 2 - renderer.getScreenWidth() / 2;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto d = getTiltDiagnostics();
  char line[96];

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{layout.content.x, metrics.topPadding, layout.content.width, metrics.headerHeight},
                 tr(STR_TILT_DIAGNOSTICS));
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  if (page == Page::Test) {
    renderer.drawText(UI_10_FONT_ID, x, y,
                      d.available ? tr(STR_TILT_SENSOR_DETECTED) : tr(STR_TILT_SENSOR_NOT_DETECTED));
    y += lineHeight + metrics.verticalSpacing;
    drawWrapped(tr(STR_TILT_DIAGNOSTICS_INSTRUCTION), x, y, contentWidth, lineHeight);
    y += metrics.verticalSpacing;
    snprintf(line, sizeof(line), "Peak R/L: %.0f / %.0f  Target: %.0f", d.rightPeakDps, d.leftPeakDps,
             d.triggerThresholdDps);
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, y, line, true, centerOffset);
    y += lineHeight + metrics.verticalSpacing;
    renderer.drawText(UI_10_FONT_ID, x, y,
                      d.rightCrossings > 0 ? tr(STR_TILT_RIGHT_DETECTED) : tr(STR_TILT_RIGHT_WAITING));
    y += lineHeight;
    renderer.drawText(UI_10_FONT_ID, x, y,
                      d.leftCrossings > 0 ? tr(STR_TILT_LEFT_DETECTED) : tr(STR_TILT_LEFT_WAITING));
    y += lineHeight + metrics.verticalSpacing;
    drawWrapped(recommendation(), x, y, contentWidth, lineHeight);
  } else {
    renderer.drawText(UI_10_FONT_ID, x, y, tr(STR_TILT_DIAGNOSTICS_MEASUREMENTS));
    y += lineHeight + metrics.verticalSpacing;
    snprintf(line, sizeof(line), "Device: X3  IMU: QMI8658  I2C: 0x%02X", d.i2cAddress);
    renderer.drawText(UI_10_FONT_ID, x, y, line);
    y += lineHeight;
    snprintf(line, sizeof(line), "State: %s  Axis: %s  Value: %.1f", d.awake ? "Awake" : "Sleep", d.axisName,
             d.currentAxisDps);
    renderer.drawText(UI_10_FONT_ID, x, y, line);
    y += lineHeight;
    snprintf(line, sizeof(line), "A: %.2f %.2f %.2f g", d.sample.ax, d.sample.ay, d.sample.az);
    renderer.drawText(UI_10_FONT_ID, x, y, line);
    y += lineHeight;
    snprintf(line, sizeof(line), "G: %.1f %.1f %.1f deg/s", d.sample.gx, d.sample.gy, d.sample.gz);
    renderer.drawText(UI_10_FONT_ID, x, y, line);
    y += lineHeight;
    snprintf(line, sizeof(line), "Trigger: %.0f  Neutral: %.0f deg/s", d.triggerThresholdDps, d.neutralThresholdDps);
    renderer.drawText(UI_10_FONT_ID, x, y, line);
    y += lineHeight;
    snprintf(line, sizeof(line), "Rate peak R/L: %.1f / %.1f", d.rightPeakDps, d.leftPeakDps);
    renderer.drawText(UI_10_FONT_ID, x, y, line);
    y += lineHeight;
    snprintf(line, sizeof(line), "Idle noise: %.1f  I2C errors: %lu", d.idleNoiseDps,
             static_cast<unsigned long>(d.readErrorCount));
    renderer.drawText(UI_10_FONT_ID, x, y, line);
  }

  if (saveResult != SaveResult::None) {
    y += metrics.verticalSpacing;
    drawWrapped(saveResult == SaveResult::Saved ? tr(STR_DIAGNOSTICS_REPORT_SAVED) : tr(STR_DIAGNOSTICS_REPORT_FAILED),
                x, y, contentWidth, lineHeight, 1);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SAVE), tr(STR_RETRY), tr(STR_NEXT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
