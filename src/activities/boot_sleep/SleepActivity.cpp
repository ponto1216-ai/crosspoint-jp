#include "SleepActivity.h"

#include <Epub.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalRTC.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include <ctime>
#include <memory>
#include <new>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"

namespace {
constexpr char TRANSPARENT_SLEEP_OVERLAY_PATH[] = "/sleep-overlay.bmp";
constexpr char TRANSPARENT_SLEEP_OVERLAY_DIR[] = "/.sleep-overlay";
constexpr size_t MAX_SLEEP_OVERLAY_FILE_NAME = 256;
constexpr uint8_t MIN_VISIBLE_ALPHA = 16;

uint16_t readLe16(FsFile& file) {
  const int low = file.read();
  const int high = file.read();
  if (low < 0 || high < 0) return 0;
  return static_cast<uint16_t>(low) | (static_cast<uint16_t>(high) << 8);
}

uint32_t readLe32(FsFile& file) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    const int byte = file.read();
    if (byte < 0) return 0;
    value |= static_cast<uint32_t>(byte) << (i * 8);
  }
  return value;
}

uint8_t alphaThreshold(const int x, const int y) {
  static constexpr uint8_t bayer4x4[16] = {0, 128, 32, 160, 192, 64, 224, 96,
                                            48, 176, 16, 144, 240, 112, 208, 80};
  return bayer4x4[((y & 3) << 2) | (x & 3)];
}

bool findNextTransparentOverlayBmp(FsFile& directory, char* name, const size_t nameSize) {
  for (auto file = directory.openNextFile(); file; file = directory.openNextFile()) {
    if (file.isDirectory()) continue;
    file.getName(name, nameSize);
    if (name[0] == '\0' || name[0] == '.') continue;
    if (!FsHelpers::hasBmpExtension(std::string(name))) continue;

    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) return true;
  }
  return false;
}
}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();

  const bool transparentOverlay =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM;

  // Show popup with reader orientation only when going to sleep from reader
  if (APP_STATE.lastSleepFromReader) {
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    if (!transparentOverlay) GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  } else if (!transparentOverlay) {
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  }

  // Sleep screen has its own inversion logic (invertScreen), independent of
  // the renderer's dark mode.  Disable dark mode here to prevent double
  // inversion artifacts; restore afterwards in case the device doesn't sleep.
  const bool wasDarkMode = renderer.isDarkMode();
  renderer.setDarkMode(false);

  if (transparentOverlay) {
    const bool rendered = renderTransparentSleepOverlay();
    renderer.setDarkMode(wasDarkMode);
    if (rendered) return;
    // Invalid or missing overlays fall back to the normal sleep screen.
  }

  // カレンダーは、実際にRTCを検出できた端末だけで有効にする。
  calendarPending = SETTINGS.rtcEnabled && SETTINGS.sleepCalendar && isTimeValid() && halRTC.isAvailable();

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      renderBlankSleepScreen();
      break;
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      renderCustomSleepScreen();
      break;
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      renderCoverSleepScreen();
      break;
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      if (APP_STATE.lastSleepFromReader) {
        renderCoverSleepScreen();
      } else {
        renderCustomSleepScreen();
      }
      break;
    default:
      renderDefaultSleepScreen();
      break;
  }

  // デバッグ: スリープ時の時刻とバッテリー情報を画面に表示（設定→本体→デバッグ表示で有効化）
  // SRC= 1:DS3231, 3:ESP-IDF, 5:None
  if (SETTINGS.debugDisplay) {
    extern uint8_t g_timeRestoreSource;
    const time_t now = time(nullptr);
    struct tm ti;
    localtime_r(&now, &ti);
    char dbg[128];
    snprintf(dbg, sizeof(dbg), "%d/%d/%d %d:%02d:%02d SRC:%d BAT:%d%%", ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec, g_timeRestoreSource, powerManager.getBatteryPercentage());
    renderer.fillRect(5, 5, 600, 30, false);
    renderer.drawText(UI_10_FONT_ID, 10, 10, dbg, true);
    renderer.displayBuffer(HalDisplay::SLEEP_REFRESH);
  }

  // ビットマップパスで消費されなかった場合（BLANK/DARK/LIGHT）はここで描画
  if (calendarPending) {
    calendarPending = false;
    renderCalendarOverlay();
    renderer.displayBuffer(HalDisplay::SLEEP_REFRESH);
  }

  renderer.setDarkMode(wasDarkMode);
}

void SleepActivity::renderCustomSleepScreen() const {
  // Check if we have a /.sleep (preferred) or /sleep directory
  const char* sleepDir = nullptr;
  auto dir = Storage.open("/.sleep");
  if (dir && dir.isDirectory()) {
    sleepDir = "/.sleep";
  } else {
    dir = Storage.open("/sleep");
    if (dir && dir.isDirectory()) {
      sleepDir = "/sleep";
    }
  }

  if (sleepDir) {
    std::vector<std::string> files;
    char name[500];
    // collect all valid BMP files
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) {
        continue;
      }
      file.getName(name, sizeof(name));
      auto filename = std::string(name);
      if (filename[0] == '.') {
        continue;
      }

      if (!FsHelpers::hasBmpExtension(filename)) {
        LOG_DBG("SLP", "Skipping non-.bmp file name: %s", name);
        continue;
      }
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() != BmpReaderError::Ok) {
        LOG_DBG("SLP", "Skipping invalid BMP file: %s", name);
        continue;
      }
      files.emplace_back(filename);
    }
    const auto numFiles = files.size();
    if (numFiles > 0) {
      // Pick a random wallpaper, excluding recently shown ones.
      // Window: up to SLEEP_RECENT_COUNT entries, capped at numFiles-1.
      const uint16_t fileCount = static_cast<uint16_t>(std::min(numFiles, static_cast<size_t>(UINT16_MAX)));
      const uint8_t window =
          static_cast<uint8_t>(std::min(static_cast<size_t>(APP_STATE.recentSleepFill), numFiles - 1));
      auto randomFileIndex = static_cast<uint16_t>(random(fileCount));
      for (uint8_t attempt = 0; attempt < 20 && APP_STATE.isRecentSleep(randomFileIndex, window); attempt++) {
        randomFileIndex = static_cast<uint16_t>(random(fileCount));
      }
      APP_STATE.pushRecentSleep(randomFileIndex);
      APP_STATE.saveToFile();
      const auto filename = std::string(sleepDir) + "/" + files[randomFileIndex];
      FsFile file;
      if (Storage.openFileForRead("SLP", filename, file)) {
        LOG_DBG("SLP", "Randomly loading: %s/%s", sleepDir, files[randomFileIndex].c_str());
        delay(100);
        Bitmap bitmap(file, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap);
          return;
        }
      }
    }
  }
  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  FsFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  renderDefaultSleepScreen();
}

bool SleepActivity::renderTransparentSleepOverlay() const {
  // The reader page is already in the framebuffer.  Reclaiming SD font data
  // before allocating the BMP row buffer makes sleep reliable with large fonts.
  if (auto* fontCaches = renderer.getFontCacheManager()) fontCaches->releaseSdFontCaches();

  // An explicitly configured root image takes priority over random overlays.
  if (renderTransparentSleepOverlayFile(TRANSPARENT_SLEEP_OVERLAY_PATH)) return true;

  auto directory = Storage.open(TRANSPARENT_SLEEP_OVERLAY_DIR);
  if (!directory || !directory.isDirectory()) return false;

  char name[MAX_SLEEP_OVERLAY_FILE_NAME];
  uint16_t count = 0;
  while (count < UINT16_MAX && findNextTransparentOverlayBmp(directory, name, sizeof(name))) ++count;
  if (count == 0) return false;

  const uint16_t selected = static_cast<uint16_t>(random(count));
  directory.rewindDirectory();
  for (uint16_t index = 0; index <= selected; ++index) {
    if (!findNextTransparentOverlayBmp(directory, name, sizeof(name))) return false;
  }

  const std::string path = std::string(TRANSPARENT_SLEEP_OVERLAY_DIR) + "/" + name;
  return renderTransparentSleepOverlayFile(path.c_str());
}

bool SleepActivity::renderTransparentSleepOverlayFile(const char* path) const {
  FsFile file;
  if (!Storage.openFileForRead("SLP", path, file)) return false;

  if (readLe16(file) != 0x4d42) return false;
  file.seekCur(8);
  const uint32_t dataOffset = readLe32(file);
  const uint32_t dibSize = readLe32(file);
  const int32_t width = static_cast<int32_t>(readLe32(file));
  const int32_t rawHeight = static_cast<int32_t>(readLe32(file));
  const uint16_t planes = readLe16(file);
  const uint16_t bitsPerPixel = readLe16(file);
  const uint32_t compression = readLe32(file);
  if (dibSize < 40 || width <= 0 || rawHeight == 0 || planes != 1 || bitsPerPixel != 32 ||
      (compression != 0 && compression != 3)) {
    // A conventional BMP is also useful as an overlay: drawBitmap leaves its
    // white pixels unchanged when the background is preserved.
    if (!file.seek(0)) return false;
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;
    renderBitmapSleepScreen(bitmap, true);
    return true;
  }

  const bool topDown = rawHeight < 0;
  const int32_t height = topDown ? -rawHeight : rawHeight;
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  if (width > screenWidth || height > screenHeight) {
    LOG_ERR("SLP", "Overlay is larger than screen: %s (%ldx%ld)", path, static_cast<long>(width),
            static_cast<long>(height));
    return false;
  }

  const uint32_t rowBytes = static_cast<uint32_t>(width) * 4u;
  auto row = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[rowBytes]);
  if (!row || !file.seek(dataOffset)) {
    LOG_ERR("SLP", "Could not allocate or seek transparent overlay");
    return false;
  }

  // Conventional 32-bit BMPs often have no usable alpha channel.  Handle
  // those through the normal white-is-transparent bitmap path instead.
  bool hasVisibleAlpha = false;
  bool hasTransparentAlpha = false;
  for (int32_t sourceY = 0; sourceY < height; ++sourceY) {
    if (file.read(row.get(), rowBytes) != static_cast<int>(rowBytes)) return false;
    for (int32_t x = 0; x < width; ++x) {
      const uint8_t alpha = row[static_cast<size_t>(x) * 4u + 3u];
      hasVisibleAlpha |= alpha >= MIN_VISIBLE_ALPHA;
      hasTransparentAlpha |= alpha < 255;
    }
  }
  if (!hasVisibleAlpha || !hasTransparentAlpha) {
    if (!file.seek(0)) return false;
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;
    renderBitmapSleepScreen(bitmap, true);
    return true;
  }
  if (!file.seek(dataOffset)) return false;

  const int originX = (screenWidth - width) / 2;
  const int originY = (screenHeight - height) / 2;
  for (int32_t sourceY = 0; sourceY < height; ++sourceY) {
    if (file.read(row.get(), rowBytes) != static_cast<int>(rowBytes)) {
      LOG_ERR("SLP", "Short read in transparent overlay");
      return false;
    }
    const int screenY = originY + (topDown ? sourceY : height - 1 - sourceY);
    for (int32_t x = 0; x < width; ++x) {
      const uint8_t* pixel = row.get() + static_cast<size_t>(x) * 4u;
      const uint8_t alpha = pixel[3];
      const int screenX = originX + x;
      if (alpha < MIN_VISIBLE_ALPHA || alpha <= alphaThreshold(screenX, screenY)) continue;
      const uint8_t luminance = static_cast<uint8_t>((77u * pixel[2] + 150u * pixel[1] + 29u * pixel[0]) >> 8);
      renderer.drawPixel(screenX, screenY, luminance < 192);
    }
  }

  renderer.displayBuffer(HalDisplay::SLEEP_REFRESH);
  return true;
}

void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::SLEEP_REFRESH);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const bool preserveBackground) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    LOG_DBG("SLP", "bitmap ratio: %f, screen ratio: %f", ratio, screenRatio);
    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        LOG_DBG("SLP", "Cropping bitmap x: %f", cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      LOG_DBG("SLP", "Centering with ratio %f to y=%d", ratio, y);
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        LOG_DBG("SLP", "Cropping bitmap y: %f", cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
      LOG_DBG("SLP", "Centering with ratio %f to x=%d", ratio, x);
    }
  } else {
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  if (!preserveBackground) renderer.clearScreen();

  const bool hasGreyscale = !preserveBackground && bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  // カレンダーをBWパスに挿入（displayBuffer前）
  drawCalendarIfPending();

  renderer.displayBuffer(HalDisplay::SLEEP_REFRESH);

  if (hasGreyscale && !SETTINGS.sleepCalendar) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  if (APP_STATE.openEpubPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;

  // Check if the current book is XTC, TXT, or EPUB
  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastXtc.generateCoverBmp()) {
      LOG_ERR("SLP", "Failed to generate XTC cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath)) {
    // Handle TXT file - looks for cover image in the same folder
    Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastTxt.load()) {
      LOG_ERR("SLP", "Failed to load last TXT");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastTxt.generateCoverBmp()) {
      LOG_ERR("SLP", "No cover image found for TXT file");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastTxt.getCoverBmpPath();
  } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
    // Handle EPUB file
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    // Skip loading css since we only need metadata here
    if (!lastEpub.load(true, true)) {
      LOG_ERR("SLP", "Failed to load last epub");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastEpub.generateCoverBmp(cropped)) {
      LOG_ERR("SLP", "Failed to generate cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastEpub.getCoverBmpPath(cropped);
  } else {
    return (this->*renderNoCoverSleepScreen)();
  }

  FsFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::SLEEP_REFRESH);
}

void SleepActivity::drawCalendarIfPending() const {
  if (!calendarPending) return;
  calendarPending = false;
  renderCalendarOverlay();
}

bool SleepActivity::isTimeValid() {
  // 2024-01-01 00:00:00 UTC = 1704067200
  // NTP未同期の場合、time()はエポック付近（1970年）を返す
  return time(nullptr) >= 1704067200;
}

void SleepActivity::renderCalendarOverlay() const {
  // 現在時刻を取得
  const time_t now = time(nullptr);
  struct tm timeInfo;
  localtime_r(&now, &timeInfo);

  const int year = timeInfo.tm_year + 1900;
  const int month = timeInfo.tm_mon + 1;
  const int today = timeInfo.tm_mday;

  // 今月の日数を計算（翌月0日 = 今月末日）
  struct tm endOfMonth = {};
  endOfMonth.tm_year = timeInfo.tm_year;
  endOfMonth.tm_mon = timeInfo.tm_mon + 1;
  endOfMonth.tm_mday = 0;
  mktime(&endOfMonth);
  const int daysInMonth = endOfMonth.tm_mday;

  // 今月1日の曜日（0=日曜）
  struct tm firstDay = {};
  firstDay.tm_year = timeInfo.tm_year;
  firstDay.tm_mon = timeInfo.tm_mon;
  firstDay.tm_mday = 1;
  mktime(&firstDay);
  const int startDow = firstDay.tm_wday;

  // レイアウト定数（フォント: UI_12 / UI_10）
  static constexpr int COL_WIDTH = 58;
  static constexpr int ROW_HEIGHT = 50;
  static constexpr int HEADER_HEIGHT = 54;
  static constexpr int DOW_HEIGHT = 34;
  static constexpr int PADDING_X = 18;
  static constexpr int PADDING_TOP = 22;
  static constexpr int PADDING_BOTTOM = 28;
  static constexpr int GRID_WIDTH = COL_WIDTH * 7;  // 406px

  // 今日マーカー: 角丸四角形
  static constexpr int TODAY_MARK_W = 40;
  static constexpr int TODAY_MARK_H = 36;
  static constexpr int TODAY_MARK_RADIUS = 8;

  // 行数を計算
  const int totalCells = startDow + daysInMonth;
  const int numRows = (totalCells + 6) / 7;

  const int calendarHeight = PADDING_TOP + HEADER_HEIGHT + DOW_HEIGHT + ROW_HEIGHT * numRows + PADDING_BOTTOM;
  const int calendarWidth = GRID_WIDTH + PADDING_X * 2;

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // X位置: 画面中央
  const int calX = (screenWidth - calendarWidth) / 2;

  // Y位置: 設定に応じて上部/中央/下部
  int calY;
  int viewTop, viewRight, viewBottom, viewLeft;
  renderer.getOrientedViewableTRBL(&viewTop, &viewRight, &viewBottom, &viewLeft);

  switch (SETTINGS.sleepCalendarPosition) {
    case CrossPointSettings::CALENDAR_POS_TOP:
      calY = viewTop + 20;
      break;
    case CrossPointSettings::CALENDAR_POS_BOTTOM:
      calY = screenHeight - viewBottom - calendarHeight - 20;
      break;
    case CrossPointSettings::CALENDAR_POS_CENTER:
    default:
      calY = (screenHeight - calendarHeight) / 2;
      break;
  }

  // 白背景角丸矩形を描画
  static constexpr int BG_CORNER_RADIUS = 16;
  renderer.fillRoundedRect(calX, calY, calendarWidth, calendarHeight, BG_CORNER_RADIUS, Color::White);

  // 年月ヘッダー: 月数字のみ中央配置
  char monthBuf[4];
  snprintf(monthBuf, sizeof(monthBuf), "%d", month);

  const int headerY = calY + PADDING_TOP - 20;
  renderer.drawCenteredText(NOTOSANS_18_FONT_ID, headerY, monthBuf, true, EpdFontFamily::REGULAR);

  // 曜日ヘッダー（UI_10, Bold）
  static constexpr StrId dowIds[] = {StrId::STR_SUN, StrId::STR_MON, StrId::STR_TUE, StrId::STR_WED,
                                     StrId::STR_THU, StrId::STR_FRI, StrId::STR_SAT};
  const int dowY = headerY + HEADER_HEIGHT;
  const int gridLeft = calX + PADDING_X;

  for (int col = 0; col < 7; col++) {
    const char* dowStr = I18N.get(dowIds[col]);
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, dowStr, EpdFontFamily::BOLD);
    const int cellCenterX = gridLeft + col * COL_WIDTH + (COL_WIDTH - textW) / 2;
    renderer.drawText(UI_10_FONT_ID, cellCenterX, dowY, dowStr, true, EpdFontFamily::BOLD);
  }

  // 日付グリッド（UI_12）
  const int gridStartY = dowY + DOW_HEIGHT;
  const int lineH = renderer.getLineHeight(UI_12_FONT_ID);

  for (int day = 1; day <= daysInMonth; day++) {
    const int cellIndex = startDow + day - 1;
    const int row = cellIndex / 7;
    const int col = cellIndex % 7;
    const bool isSunday = (col == 0);
    const bool isToday = (day == today);

    char dayBuf[4];
    snprintf(dayBuf, sizeof(dayBuf), "%d", day);

    const auto style = (isSunday || isToday) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int textW = renderer.getTextWidth(UI_12_FONT_ID, dayBuf, style);
    const int cellCenterX = gridLeft + col * COL_WIDTH + COL_WIDTH / 2;
    const int cellCenterY = gridStartY + row * ROW_HEIGHT + ROW_HEIGHT / 2;
    // drawTextのY座標はグリフ上端。lineHeightで垂直中央揃え。
    const int textX = cellCenterX - textW / 2;
    const int textY = cellCenterY - lineH / 2;

    if (isToday) {
      // 薄グレー角丸四角（テキストと同じ中心基準）+ 黒文字
      renderer.fillRoundedRect(cellCenterX - TODAY_MARK_W / 2, textY - (TODAY_MARK_H - lineH) / 2 + 2, TODAY_MARK_W,
                               TODAY_MARK_H, TODAY_MARK_RADIUS, Color::LightGray);
      renderer.drawText(UI_12_FONT_ID, textX, textY, dayBuf, true, style);
    } else {
      renderer.drawText(UI_12_FONT_ID, textX, textY, dayBuf, true, style);
    }
  }
}
