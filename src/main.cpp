#include <Arduino.h>
#include <BatteryMonitor.h>
#include <BoardConfig.h>
#include <Epub.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FontManager.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalRTC.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <Wire.h>
#include <XteinkDetect.h>
#include <builtinFonts/all.h>
#include <esp_task_wdt.h>
#include <sys/time.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OrientationHelper.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#ifdef SIMULATOR
#include "activities/settings/SettingsActivity.h"
#endif
#ifdef GRAYSCALE_TEST_MODE
#include "activities/util/GrayscaleTestActivity.h"
#endif
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

// デバッグ: 起動時にどの時刻復元ブランチが使われたかを記録
// 0=未設定, 1=DS3231, 3=ESP-IDF, 5=なし
uint8_t g_timeRestoreSource = 0;

MappedInputManager mappedInputManager(gpio);
GfxRenderer renderer(display);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());

void logX3DisplayProbeDiag() {
#ifdef SIMULATOR
  return;
#else
  if (!gpio.deviceIsX3()) return;

  const auto& diag = freeink::getXteinkDisplayProbeDiag();
  if (!diag.valid) {
    LOG_ERR("XTDET", "X3 display-controller probe did not run");
    return;
  }

  const char* verdict = "inconclusive";
  if (diag.verdict == static_cast<uint8_t>(freeink::DisplayControllerVerdict::PrimaryAssumed)) {
    verdict = "UC8253 assumed";
  } else if (diag.verdict == static_cast<uint8_t>(freeink::DisplayControllerVerdict::Uc81xxConfirmed)) {
    verdict = "UC8279 confirmed";
  }

  LOG_INF("XTDET", "VER=%02X %02X %02X %02X %02X FLG=%02X -> %s promoted=%d", diag.ver[0], diag.ver[1], diag.ver[2],
          diag.ver[3], diag.ver[4], diag.flg, verdict, diag.promoted ? 1 : 0);
  if (diag.mtpValid) {
    LOG_INF("XTDET", "MTP key=%02X product=%02X %02X %02X LUT=%02X %02X %02X %02X", diag.mtp[0], diag.mtp[0x17],
            diag.mtp[0x18], diag.mtp[0x19], diag.mtp[0x1A], diag.mtp[0x1B], diag.mtp[0x1C], diag.mtp[0x1D]);
  }
#endif
}

// Fonts
#ifndef OMIT_FONTS
EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

#endif  // OMIT_FONTS

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

static const char* wakeupReasonName(const HalGPIO::WakeupReason reason) {
  switch (reason) {
    case HalGPIO::WakeupReason::PowerButton:
      return "power_button";
    case HalGPIO::WakeupReason::AfterFlash:
      return "after_flash";
    case HalGPIO::WakeupReason::AfterUSBPower:
      return "after_usb";
    case HalGPIO::WakeupReason::Other:
    default:
      return "other";
  }
}

// デバッグ表示有効時、バッテリーログをSDカードに追記（/.crosspoint/power_log.txt）
static void appendPowerLog(const char* event, const char* wakeReason = "-") {
  if (!SETTINGS.debugDisplay) return;
  const time_t now = time(nullptr);
  struct tm ti;
  localtime_r(&now, &ti);

  int rawAdc = -1;
  int voltageMv = -1;
  int voltagePercent = -1;
  int currentMa = -32769;
  if (gpio.deviceIsX3()) {
    // BQ27220から電圧(mV)と電流(mA)を読み取り。
    uint16_t gaugeVoltageMv = 0;
    Wire.beginTransmission(0x55);
    Wire.write(0x08);  // BQ27220_VOLT_REG
    if (Wire.endTransmission(false) == 0 &&
        Wire.requestFrom(static_cast<uint8_t>(0x55), static_cast<uint8_t>(2)) == 2) {
      gaugeVoltageMv = Wire.read() | (static_cast<uint16_t>(Wire.read()) << 8);
      voltageMv = gaugeVoltageMv;
    }
    Wire.beginTransmission(0x55);
    Wire.write(0x0C);  // BQ27220_CUR_REG
    if (Wire.endTransmission(false) == 0 &&
        Wire.requestFrom(static_cast<uint8_t>(0x55), static_cast<uint8_t>(2)) == 2) {
      currentMa = static_cast<int16_t>(Wire.read() | (static_cast<uint16_t>(Wire.read()) << 8));
    }
  } else {
    // X4 has no fuel gauge. Preserve both the raw ADC reading and calibrated
    // battery voltage so percentage-table errors can be separated from real drain.
    rawAdc = analogRead(BAT_GPIO0);
    const BatteryMonitor battery;
    voltageMv = battery.readMillivolts();
    if (voltageMv > 0) voltagePercent = BatteryMonitor::percentageFromMillivolts(voltageMv);
  }

  const int displayedPercent = powerManager.getBatteryPercentage();
  const bool usbConnected = gpio.isUsbConnected();
  char line[256];
  snprintf(line, sizeof(line),
           "%04d/%02d/%02d %02d:%02d:%02d event=%s uptime_ms=%lu device=%s ui_pct=%d voltage_pct=%d "
           "raw_adc=%d voltage_mV=%d current_mA=%d usb=%d wake=%s rtc=%s\n",
           ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour, ti.tm_min, ti.tm_sec, event, millis(),
           gpio.deviceIsX3() ? "X3" : "X4", displayedPercent, voltagePercent, rawAdc, voltageMv, currentMa,
           usbConnected ? 1 : 0, wakeReason, SETTINGS.rtcEnabled ? "ON" : "OFF");
  LOG_INF("BAT", "%s", line);
  auto file = Storage.open("/.crosspoint/power_log.txt", O_WRONLY | O_CREAT | O_APPEND);
  if (file) {
    file.write(line, strlen(line));
    file.close();
  }
}

// Enter deep sleep mode
void enterDeepSleep() {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();
  APP_STATE.saveToFile();
  appendPowerLog("SLEEP");

  activityManager.goToSleep();

  // Native SDMMC boards must unmount and release their host before the sleep
  // power rails are isolated.  The SDK keeps this a no-op on X3/X4 SPI cards.
#ifndef SIMULATOR
  Storage.shutdown();
#endif

  halTiltSensor.deepSleep();
  display.deepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  // Only a detected RTC can justify retaining the sleep domain.  This applies
  // to X3 today and to the S3 X4 family when its BoardConfig profile supplies
  // an RTC.
  const bool fullPowerOff = !SETTINGS.rtcEnabled || !halRTC.isAvailable();
  powerManager.startDeepSleep(gpio, fullPowerOff);
}

void ensureSdFontLoaded(bool isVertical) { sdFontSystem.ensureLoaded(renderer, isVertical); }

void configureRubyFont(const bool isVertical) {
  const auto& ds = SETTINGS.getDirectionSettings(isVertical);
  if (!ds.rubyEnabled) {
    TextBlock::rubyFontId = 0;
    return;
  }

  static constexpr uint8_t RUBY_FONT_SIZE_ENUM = 5;  // 8pt
  int rubyId = 0;
  if (ds.sdFontFamilyName[0] != '\0' && SETTINGS.sdFontIdResolver) {
    rubyId = SETTINGS.sdFontIdResolver(SETTINGS.sdFontResolverCtx, ds.sdFontFamilyName, RUBY_FONT_SIZE_ENUM);
  }
  TextBlock::rubyFontId = rubyId != 0 ? rubyId : SETTINGS.getReaderFontId(isVertical);
  LOG_DBG("RUBY", "Configured ruby font: vertical=%d fontId=%d", isVertical ? 1 : 0, TextBlock::rubyFontId);
}

void configureSmallFont(const bool isVertical) {
  TextBlock::smallFontId = SETTINGS.getTableFontId(isVertical);
  LOG_DBG("SCRIPT", "Configured small font: vertical=%d fontId=%d", isVertical ? 1 : 0, TextBlock::smallFontId);
}

static bool bootRecoveryMode = false;
static uint8_t bootRecoveryButton = HalGPIO::BTN_UP;

bool isBootRecoveryChordHeld() {
  if (gpio.isPressed(HalGPIO::BTN_POWER) || gpio.isPressed(bootRecoveryButton)) return true;
  // X4 revisions use the same ADC ladder but the physical order of its two
  // side keys has not been consistent in the field.  Keep both suppressed
  // after a successful recovery entry so neither becomes a picker action.
  return !gpio.deviceIsX3() && (gpio.isPressed(HalGPIO::BTN_UP) || gpio.isPressed(HalGPIO::BTN_DOWN));
}

void setupDisplayAndFonts(bool recoveryOnly = false) {
  display.begin();
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);
  // Recovery must render without reading user-selected or damaged SD fonts.
  if (recoveryOnly) return;

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
#endif  // OMIT_FONTS

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);

  LOG_DBG("MAIN", "Fonts setup");
}

// SPIピンを明示的にリセットする。ディープスリープ中にSDカードを抜き差しした場合、
// SPIバスが不定状態になり sd.begin() がハングする可能性がある（Issue #23）。
static void resetSpiPins() {
#if FREEINK_MCU_C3
  constexpr gpio_num_t spiPins[] = {
      GPIO_NUM_7,   // SPI_MISO (SD/Display共有)
      GPIO_NUM_8,   // EPD_SCLK
      GPIO_NUM_10,  // EPD_MOSI
      GPIO_NUM_12,  // SD_CS
  };
  for (auto pin : spiPins) {
    gpio_reset_pin(pin);
  }
#endif
}

void setup() {
  // タイムゾーン設定（JST = UTC+9）。ディープスリープ後のリブートでも
  // localtime_r()が日本時間を返すようにするため、起動直後に設定する。
  setenv("TZ", "JST-9", 1);
  tzset();

  resetSpiPins();
  HalSystem::begin();
  gpio.begin();
  powerManager.begin();
  halRTC.begin();
  halTiltSensor.begin();

#ifdef ENABLE_SERIAL_LOG
  // X3 infers USB connection from charging current.  At full charge that
  // current becomes zero even while USB is attached, so conditional Serial
  // initialization prevents flashing and diagnostic logs.  Initialize USB CDC
  // unconditionally and keep writes non-blocking when no host is connected.
  delay(250);
  Serial.begin(115200);
  logSerial.setTxTimeoutMs(1);
#endif

  LOG_INF("MAIN", "Hardware profile: %s", BoardConfig::ACTIVE.name);
  logX3DisplayProbeDiag();

  // InputManager's debounced state takes about 500ms to settle after boot.
  // X3 has one established recovery key (BTN_UP). X4 uses the same ADC ladder,
  // but field units do not consistently expose its physical upper key as the
  // same logical button, so either X4 side key is an intentional recovery key.
  // Do not gate the X4 chord on the reset classification: its power latch can
  // report Other despite a physical POWER-button start.
  const auto wakeupReason = gpio.getWakeupReason();
  const unsigned long settleStart = millis();
  while (millis() - settleStart < 500) {
    gpio.update();
    delay(10);
  }
  const bool upHeld = gpio.isPressed(HalGPIO::BTN_UP);
  const bool downHeld = gpio.isPressed(HalGPIO::BTN_DOWN);
  if (gpio.deviceIsX3()) {
    bootRecoveryButton = HalGPIO::BTN_UP;
    bootRecoveryMode = wakeupReason == HalGPIO::WakeupReason::PowerButton && upHeld;
  } else if (upHeld || downHeld) {
    bootRecoveryButton = upHeld ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN;
    bootRecoveryMode = true;
  }
  LOG_INF("MAIN", "Recovery input: wake=%d power=%d up=%d down=%d x3=%d", static_cast<int>(wakeupReason),
          gpio.isPressed(HalGPIO::BTN_POWER) ? 1 : 0, upHeld ? 1 : 0, downHeld ? 1 : 0, gpio.deviceIsX3() ? 1 : 0);
  if (bootRecoveryMode) {
    LOG_INF("MAIN", "Boot recovery: POWER + side button");
  }

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  // WDTガード: sd.begin()がSPIハングした場合、5秒で自動再起動する（Issue #23）
  static const esp_task_wdt_config_t wdtConfig = {
      .timeout_ms = 5000,
      .idle_core_mask = 0,
      .trigger_panic = true,
  };
  esp_task_wdt_init(&wdtConfig);
  esp_task_wdt_add(NULL);
  const bool sdOk = Storage.begin();
  esp_task_wdt_delete(NULL);
  esp_task_wdt_deinit();
  if (bootRecoveryMode) {
    LOG_INF("MAIN", "Boot recovery: skipping user state and SD fonts");
    ButtonNavigator::setMappedInputManager(mappedInputManager);
    UITheme::getInstance().setTheme(CrossPointSettings::UI_THEME::CLASSIC);
    setupDisplayAndFonts(true);
    if (sdOk) {
      activityManager.replaceActivity(std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, true));
    } else {
      activityManager.goToFullScreenMessage("SD card error - hold POWER to restart", EpdFontFamily::BOLD);
    }
    activityManager.loop();
    // Do not let the entry chord also move the picker or trigger a restart.
    while (isBootRecoveryChordHeld()) {
      delay(50);
      gpio.update();
    }
    gpio.update();
    return;
  }
  if (!sdOk) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts();
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  SETTINGS.loadFromFile();
  I18N.loadSettings();
#ifdef SIMULATOR
  if (const char* language = std::getenv("CROSSPOINT_SIM_LANGUAGE")) {
    if (std::strcmp(language, "ja") == 0) {
      I18N.setLanguage(Language::JAPANESE);
    } else if (std::strcmp(language, "en") == 0) {
      I18N.setLanguage(Language::EN);
    }
  }
#endif
  UITheme::getInstance().reload();
#ifdef SIMULATOR
  // Layout QA starts directly on Settings. Apply its requested orientation
  // before SDL creates the window so screenshot dimensions are deterministic.
  if (std::getenv("CROSSPOINT_SIM_SETTINGS_CATEGORY")) {
    GfxRenderer::Orientation qaOrientation = GfxRenderer::Orientation::Portrait;
    switch (static_cast<CrossPointSettings::UI_ORIENTATION>(SETTINGS.uiOrientation)) {
      case CrossPointSettings::UI_ORIENTATION::UI_INVERTED:
        qaOrientation = GfxRenderer::Orientation::PortraitInverted;
        break;
      case CrossPointSettings::UI_ORIENTATION::UI_LANDSCAPE_CW:
        qaOrientation = GfxRenderer::Orientation::LandscapeCounterClockwise;
        break;
      case CrossPointSettings::UI_ORIENTATION::UI_LANDSCAPE_CCW:
        qaOrientation = GfxRenderer::Orientation::LandscapeClockwise;
        break;
      default:
        break;
    }
    renderer.setOrientation(qaOrientation);
    mappedInputManager.setEffectiveOrientation(OrientationHelper::toInputOrientation(qaOrientation));
  }
#endif
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  // CJK: External font system
  renderer.setReaderFallbackFontId(SETTINGS.getBuiltInReaderFontId(false));
  FontManager::getInstance().scanFonts();
  FontManager::getInstance().loadSettings();

  // CJK: Dark mode
  renderer.setDarkMode(SETTINGS.colorMode == CrossPointSettings::COLOR_MODE::DARK_MODE);
  renderer.setInvertImagesInDarkMode(SETTINGS.invertImages);

  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      LOG_DBG("MAIN", "Verifying power button press duration");
#ifdef SIMULATOR
      gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                   SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
#else
      gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                   SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP,
                                   !SETTINGS.rtcEnabled || !halRTC.isAvailable());
#endif
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      powerManager.startDeepSleep(gpio, !SETTINGS.rtcEnabled || !halRTC.isAvailable());
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_INF("MAIN", "Starting Yomuka version " CROSSPOINT_VERSION " (build " CROSSPOINT_BUILD_ID ")");

  setupDisplayAndFonts();

#ifdef GRAYSCALE_TEST_MODE
  activityManager.replaceActivity(std::make_unique<GrayscaleTestActivity>(renderer, mappedInputManager));
  return;
#endif

  activityManager.goToBoot();

  APP_STATE.loadFromFile();

  // 時刻復元の優先順位:
  // 1. BoardConfigで定義された外部RTC
  // 2. ESP-IDF内部復元（CONFIG_NEWLIB_TIME_SYSCALL_USE_RTC_HRT、USB給電時のみ有効）
  // 時刻が不明な場合はエポック付近のまま残し、isTimeValid()がfalseを返すようにする。
  {
    const time_t bootTime = time(nullptr);
    struct tm rtcTm;
    if (halRTC.readTime(rtcTm)) {
      // 外部RTCからUTC時刻を復元
      // timegm() が利用できないため、TZを一時的にUTCに変更してmktime()を使用
      setenv("TZ", "UTC0", 1);
      tzset();
      const time_t rtcTime = mktime(&rtcTm);
      setenv("TZ", "JST-9", 1);
      tzset();
      if (rtcTime >= 1704067200) {
        struct timeval tv = {.tv_sec = rtcTime, .tv_usec = 0};
        settimeofday(&tv, nullptr);
        g_timeRestoreSource = 1;
        LOG_DBG("MAIN", "Restored time from RTC: %ld (boot=%ld)", (long)rtcTime, (long)bootTime);
      } else {
        LOG_DBG("MAIN", "RTC time too old: %ld", (long)rtcTime);
      }
    } else if (bootTime >= 1704067200) {
      g_timeRestoreSource = 3;
      LOG_DBG("MAIN", "Using ESP-IDF restored time: %ld", (long)bootTime);
    } else {
      g_timeRestoreSource = 5;
      LOG_DBG("MAIN", "No valid time source available");
    }
  }

  appendPowerLog("WAKE", wakeupReasonName(wakeupReason));

  RECENT_BOOKS.loadFromFile();

#ifdef SIMULATOR
  if (const char* categoryValue = std::getenv("CROSSPOINT_SIM_SETTINGS_CATEGORY")) {
    const int category = std::clamp(std::atoi(categoryValue), 0, 4);
    activityManager.replaceActivity(
        std::make_unique<SettingsActivity>(renderer, mappedInputManager, nullptr, category, 0));
  } else
#endif
      if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path);
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  mappedInputManager.update();
  if (bootRecoveryMode) {
    // Normal sleep saves APP_STATE and uses the normal sleep screen. Keep
    // recovery isolated from both; a long POWER press restarts without writes.
    if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() > 1500) ESP.restart();
    activityManager.loop();
    delay(10);
    return;
  }
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

  renderer.setFadingFix(SETTINGS.fadingFix);

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || halTiltSensor.hadActivity() ||
      activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  // Reserve a brief interval after POWER is pressed for the screenshot chord.
  // With short POWER set to Sleep its normal threshold is only 10 ms, which
  // otherwise sends the device to sleep before DOWN can be pressed.
  constexpr unsigned long screenshotChordGraceMs = 250;
  static bool screenshotChordPending = false;
  static bool screenshotChordActive = false;
  static unsigned long screenshotPowerPressedAt = 0;

  if (screenshotChordActive) {
    // Consume both release edges so a completed screenshot cannot also become
    // a short POWER action or a reader page turn.
    if (gpio.isPressed(HalGPIO::BTN_POWER) || gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    screenshotChordActive = false;
    return;
  }

  if (gpio.wasPressed(HalGPIO::BTN_POWER)) {
    screenshotChordPending = true;
    screenshotPowerPressedAt = millis();
  }

  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotChordPending = false;
    screenshotChordActive = true;
    {
      RenderLock lock;
      ScreenshotUtil::takeScreenshot(renderer);
    }
    return;
  }

  if (screenshotChordPending) {
    if (!gpio.isPressed(HalGPIO::BTN_POWER)) {
      screenshotChordPending = false;
      // Preserve the configured short-press sleep behavior when DOWN was not
      // added during the grace interval.
      if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) {
        enterDeepSleep();
        return;
      }
    } else if (millis() - screenshotPowerPressedAt < screenshotChordGraceMs) {
      return;
    } else {
      screenshotChordPending = false;
    }
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    RenderLock lock;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
