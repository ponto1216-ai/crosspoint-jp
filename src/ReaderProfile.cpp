#include "ReaderProfile.h"

#include <ArduinoJson.h>
#include <FontManager.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "SdCardFontGlobals.h"

namespace {

constexpr char kProfileDirectory[] = "/.crosspoint/profiles";
constexpr char kBackupPath[] = "/.crosspoint/profiles/reader-before-load.json";
constexpr uint8_t kFormatVersion = 1;

struct ProfileData {
  DirectionSettings horizontal;
  DirectionSettings vertical;
  uint8_t writingMode;
  uint8_t orientation;
  uint8_t embeddedStyle;
  uint8_t imageRendering;
  uint8_t invertImages;
  uint8_t refreshFrequency;
  uint8_t longPressChapterSkip;
  uint8_t statusBarChapterPageCount;
  uint8_t statusBarBookProgressPercentage;
  uint8_t statusBarProgressBar;
  uint8_t statusBarProgressBarThickness;
  uint8_t statusBarTitle;
  uint8_t statusBarBattery;
  uint8_t xtcStatusBarMode;
  char externalFontFilename[64] = "";
};

std::string profilePath(const uint8_t slot) {
  return std::string(kProfileDirectory) + "/reader-profile-" + std::to_string(slot + 1) + ".json";
}

ProfileData capture() {
  ProfileData data{SETTINGS.horizontal,
                   SETTINGS.vertical,
                   SETTINGS.writingMode,
                   SETTINGS.orientation,
                   SETTINGS.embeddedStyle,
                   SETTINGS.imageRendering,
                   SETTINGS.invertImages,
                   SETTINGS.refreshFrequency,
                   SETTINGS.longPressChapterSkip,
                   SETTINGS.statusBarChapterPageCount,
                   SETTINGS.statusBarBookProgressPercentage,
                   SETTINGS.statusBarProgressBar,
                   SETTINGS.statusBarProgressBarThickness,
                   SETTINGS.statusBarTitle,
                   SETTINGS.statusBarBattery,
                   SETTINGS.xtcStatusBarMode};
  const FontInfo* externalFont = FontMgr.getFontInfo(FontMgr.getSelectedIndex());
  if (externalFont) {
    strncpy(data.externalFontFilename, externalFont->filename, sizeof(data.externalFontFilename) - 1);
  }
  return data;
}

void writeDirection(JsonObject obj, const DirectionSettings& settings) {
  obj["fontFamily"] = settings.fontFamily;
  obj["sdFontFamilyName"] = settings.sdFontFamilyName;
  obj["fontSize"] = settings.fontSize;
  obj["lineSpacing"] = settings.lineSpacing;
  obj["charSpacing"] = settings.charSpacing;
  obj["paragraphAlignment"] = settings.paragraphAlignment;
  obj["extraParagraphSpacing"] = settings.extraParagraphSpacing;
  obj["hyphenationEnabled"] = settings.hyphenationEnabled;
  obj["screenMargin"] = settings.screenMargin;
  obj["firstLineIndent"] = settings.firstLineIndent;
  obj["rubyEnabled"] = settings.rubyEnabled;
  obj["rubyOffsetX"] = settings.rubyOffsetX;
  obj["rubyOffsetY"] = settings.rubyOffsetY;
}

String serialize(const ProfileData& data) {
  JsonDocument doc;
  doc["formatVersion"] = kFormatVersion;
  doc["kind"] = "yomuka-reader-profile";
  auto reader = doc["reader"].to<JsonObject>();
  writeDirection(reader["horizontal"].to<JsonObject>(), data.horizontal);
  writeDirection(reader["vertical"].to<JsonObject>(), data.vertical);
  reader["writingMode"] = data.writingMode;
  reader["orientation"] = data.orientation;
  reader["embeddedStyle"] = data.embeddedStyle;
  reader["imageRendering"] = data.imageRendering;
  reader["invertImages"] = data.invertImages;
  reader["refreshFrequency"] = data.refreshFrequency;
  reader["longPressChapterSkip"] = data.longPressChapterSkip;
  reader["externalFontFilename"] = data.externalFontFilename;
  auto status = reader["statusBar"].to<JsonObject>();
  status["chapterPageCount"] = data.statusBarChapterPageCount;
  status["bookProgressPercentage"] = data.statusBarBookProgressPercentage;
  status["progressBar"] = data.statusBarProgressBar;
  status["progressBarThickness"] = data.statusBarProgressBarThickness;
  status["title"] = data.statusBarTitle;
  status["battery"] = data.statusBarBattery;
  status["xtcStatusBarMode"] = data.xtcStatusBarMode;
  String json;
  serializeJson(doc, json);
  return json;
}

bool readU8(JsonObjectConst obj, const char* key, const uint8_t min, const uint8_t max, uint8_t& target) {
  const JsonVariantConst value = obj[key];
  if (value.isNull() || !value.is<int>()) return false;
  const int parsed = value.as<int>();
  if (parsed < min || parsed > max) return false;
  target = static_cast<uint8_t>(parsed);
  return true;
}

bool readDirection(JsonObjectConst obj, DirectionSettings& target) {
  const JsonVariantConst family = obj["sdFontFamilyName"];
  if (family.isNull() || !family.is<const char*>()) return false;
  const char* familyName = family.as<const char*>();
  if (strlen(familyName) >= sizeof(target.sdFontFamilyName)) return false;
  if (!readU8(obj, "fontFamily", 0, CrossPointSettings::FONT_FAMILY_COUNT - 1, target.fontFamily) ||
      !readU8(obj, "fontSize", 0, CrossPointSettings::FONT_SIZE_COUNT - 1, target.fontSize) ||
      !readU8(obj, "lineSpacing", CrossPointSettings::LINE_SPACING_MIN, CrossPointSettings::LINE_SPACING_MAX,
              target.lineSpacing) ||
      !readU8(obj, "charSpacing", 0, 50, target.charSpacing) ||
      !readU8(obj, "paragraphAlignment", 0, CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT - 1,
              target.paragraphAlignment) ||
      !readU8(obj, "extraParagraphSpacing", 0, 4, target.extraParagraphSpacing) ||
      !readU8(obj, "hyphenationEnabled", 0, 1, target.hyphenationEnabled) ||
      !readU8(obj, "screenMargin", 5, 40, target.screenMargin) ||
      !readU8(obj, "firstLineIndent", 0, 1, target.firstLineIndent) ||
      !readU8(obj, "rubyEnabled", 0, 1, target.rubyEnabled)) {
    return false;
  }

  // Reader profiles created before the ruby-position fields are still valid.
  // Their absence means the same neutral position as an untouched setting:
  // user-visible offset 0, stored internally with the +16 bias.
  if (!obj["rubyOffsetX"].isNull() && !readU8(obj, "rubyOffsetX", 0, 80, target.rubyOffsetX)) return false;
  if (!obj["rubyOffsetY"].isNull() && !readU8(obj, "rubyOffsetY", 0, 80, target.rubyOffsetY)) return false;
  strncpy(target.sdFontFamilyName, familyName, sizeof(target.sdFontFamilyName) - 1);
  target.sdFontFamilyName[sizeof(target.sdFontFamilyName) - 1] = '\0';
  return true;
}

bool parse(const String& json, ProfileData& target) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  if ((doc["formatVersion"] | 0) != kFormatVersion || strcmp(doc["kind"] | "", "yomuka-reader-profile") != 0) return false;
  const JsonObjectConst reader = doc["reader"].as<JsonObjectConst>();
  const JsonObjectConst status = reader["statusBar"].as<JsonObjectConst>();
  if (reader.isNull() || status.isNull() || !readDirection(reader["horizontal"].as<JsonObjectConst>(), target.horizontal) ||
      !readDirection(reader["vertical"].as<JsonObjectConst>(), target.vertical) ||
      !readU8(reader, "writingMode", 0, CrossPointSettings::WRITING_MODE_COUNT - 1, target.writingMode) ||
      !readU8(reader, "orientation", 0, CrossPointSettings::ORIENTATION_COUNT - 1, target.orientation) ||
      !readU8(reader, "embeddedStyle", 0, CrossPointSettings::BOOK_STYLE_COUNT - 1, target.embeddedStyle) ||
      !readU8(reader, "imageRendering", 0, CrossPointSettings::IMAGE_RENDERING_COUNT - 1, target.imageRendering) ||
      !readU8(reader, "invertImages", 0, 1, target.invertImages) ||
      !readU8(reader, "refreshFrequency", 0, CrossPointSettings::REFRESH_FREQUENCY_COUNT - 1, target.refreshFrequency) ||
      !readU8(reader, "longPressChapterSkip", 0, 1, target.longPressChapterSkip) ||
      !readU8(status, "chapterPageCount", 0, 1, target.statusBarChapterPageCount) ||
      !readU8(status, "bookProgressPercentage", 0, 1, target.statusBarBookProgressPercentage) ||
      !readU8(status, "progressBar", 0, CrossPointSettings::STATUS_BAR_PROGRESS_BAR_COUNT - 1,
              target.statusBarProgressBar) ||
      !readU8(status, "progressBarThickness", 0, CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT - 1,
              target.statusBarProgressBarThickness) ||
      !readU8(status, "title", 0, CrossPointSettings::STATUS_BAR_TITLE_COUNT - 1, target.statusBarTitle) ||
      !readU8(status, "battery", 0, 1, target.statusBarBattery)) {
    return false;
  }
  // Older profiles used an on/off XTC progress-bar switch. Preserve an
  // enabled value as a bottom status bar during migration.
  if (!status["xtcStatusBarMode"].isNull()) {
    if (!readU8(status, "xtcStatusBarMode", 0, CrossPointSettings::XTC_STATUS_BAR_MODE_COUNT - 1,
                target.xtcStatusBarMode))
      return false;
  } else if (!status["xtcProgressBar"].isNull()) {
    uint8_t legacyProgressBar = 0;
    if (!readU8(status, "xtcProgressBar", 0, 1, legacyProgressBar)) return false;
    target.xtcStatusBarMode = legacyProgressBar ? CrossPointSettings::XTC_STATUS_BAR_BOTTOM
                                                 : CrossPointSettings::XTC_STATUS_BAR_HIDE;
  }
  const char* externalFontFilename = reader["externalFontFilename"] | "";
  if (strlen(externalFontFilename) >= sizeof(target.externalFontFilename)) return false;
  strncpy(target.externalFontFilename, externalFontFilename, sizeof(target.externalFontFilename) - 1);
  target.externalFontFilename[sizeof(target.externalFontFilename) - 1] = '\0';
  return true;
}

bool applyDirection(const DirectionSettings& source, DirectionSettings& destination) {
  destination = source;
  if (destination.sdFontFamilyName[0] == '\0') return false;
  // The resolver only returns an ID for the family currently loaded in the
  // renderer. During a profile switch it still points at the previous family,
  // so using it here falsely treats another installed family as missing.
  if (sdFontSystem.registry().findFamily(destination.sdFontFamilyName) != nullptr) {
    return false;
  }
  destination.fontFamily = CrossPointSettings::NOTOSANS;
  destination.sdFontFamilyName[0] = '\0';
  return true;
}

bool applyExternalFont(const ProfileData& data) {
  if (data.externalFontFilename[0] == '\0') {
    FontMgr.selectFont(-1);
    return false;
  }

  FontMgr.scanFonts();
  for (int index = 0; index < FontMgr.getFontCount(); ++index) {
    const FontInfo* font = FontMgr.getFontInfo(index);
    if (font && strcmp(font->filename, data.externalFontFilename) == 0 &&
        ExternalFont::canFitGlyph(font->width, font->height)) {
      FontMgr.selectFont(index);
      return false;
    }
  }
  FontMgr.selectFont(-1);
  return true;
}

bool apply(const ProfileData& data, bool* externalFontFallback = nullptr) {
  SETTINGS.horizontal = data.horizontal;
  SETTINGS.vertical = data.vertical;
  SETTINGS.writingMode = data.writingMode;
  SETTINGS.orientation = data.orientation;
  SETTINGS.embeddedStyle = data.embeddedStyle;
  SETTINGS.imageRendering = data.imageRendering;
  SETTINGS.invertImages = data.invertImages;
  SETTINGS.refreshFrequency = data.refreshFrequency;
  SETTINGS.longPressChapterSkip = data.longPressChapterSkip;
  SETTINGS.statusBarChapterPageCount = data.statusBarChapterPageCount;
  SETTINGS.statusBarBookProgressPercentage = data.statusBarBookProgressPercentage;
  SETTINGS.statusBarProgressBar = data.statusBarProgressBar;
  SETTINGS.statusBarProgressBarThickness = data.statusBarProgressBarThickness;
  SETTINGS.statusBarTitle = data.statusBarTitle;
  SETTINGS.statusBarBattery = data.statusBarBattery;
  SETTINGS.xtcStatusBarMode = data.xtcStatusBarMode;

  // The legacy external-font manager persists separately from
  // CrossPointSettings, so restore it explicitly. Matching by filename avoids
  // selecting a different font when the SD-card directory order changes.
  const bool missingExternalFont = applyExternalFont(data);
  if (externalFontFallback) *externalFontFallback = missingExternalFont;
  return SETTINGS.saveToFile();
}

ProfileData defaults() {
  ProfileData data{};
  data.horizontal = DirectionSettings{};
  data.vertical = {CrossPointSettings::NOTOSANS, "", CrossPointSettings::MEDIUM,
                   CrossPointSettings::LINE_SPACING_DEFAULT, 15, CrossPointSettings::JUSTIFIED, 0, 0, 10, 1, 1, 16,
                   16};
  data.writingMode = CrossPointSettings::WM_AUTO;
  data.orientation = CrossPointSettings::PORTRAIT;
  data.embeddedStyle = CrossPointSettings::BALANCED_STYLE;
  data.imageRendering = CrossPointSettings::IMAGES_DISPLAY;
  data.invertImages = 0;
  data.refreshFrequency = CrossPointSettings::REFRESH_15;
  data.longPressChapterSkip = 1;
  data.statusBarChapterPageCount = 0;
  data.statusBarBookProgressPercentage = 0;
  data.statusBarProgressBar = CrossPointSettings::HIDE_PROGRESS;
  data.statusBarProgressBarThickness = CrossPointSettings::PROGRESS_BAR_NORMAL;
  data.statusBarTitle = CrossPointSettings::BOOK_TITLE;
  data.statusBarBattery = 0;
  data.xtcStatusBarMode = CrossPointSettings::XTC_STATUS_BAR_HIDE;
  return data;
}

}  // namespace

bool ReaderProfile::save(const uint8_t slot) {
  if (slot >= SLOT_COUNT || !Storage.ready() || !Storage.ensureDirectoryExists(kProfileDirectory)) return false;
  return Storage.writeFile(profilePath(slot).c_str(), serialize(capture()));
}

ReaderProfile::LoadResult ReaderProfile::load(const uint8_t slot) {
  if (slot >= SLOT_COUNT || !Storage.ready()) return LoadResult::Missing;
  const std::string path = profilePath(slot);
  if (!Storage.exists(path.c_str())) return LoadResult::Missing;
  ProfileData candidate{};
  if (!parse(Storage.readFile(path.c_str()), candidate)) return LoadResult::Invalid;
  const ProfileData previous = capture();
  if (!Storage.ensureDirectoryExists(kProfileDirectory) || !Storage.writeFile(kBackupPath, serialize(previous))) {
    return LoadResult::BackupFailed;
  }
  const bool missingFont = applyDirection(candidate.horizontal, candidate.horizontal) |
                           applyDirection(candidate.vertical, candidate.vertical);
  bool missingExternalFont = false;
  if (!apply(candidate, &missingExternalFont)) {
    apply(previous);
    return LoadResult::SaveFailed;
  }
  const bool usedFontFallback = missingFont || missingExternalFont;
  LOG_INF("PROFILE", "Loaded reader profile %u%s", static_cast<unsigned>(slot + 1),
          usedFontFallback ? " with fallback font" : "");
  return usedFontFallback ? LoadResult::LoadedWithMissingFont : LoadResult::Loaded;
}

bool ReaderProfile::resetToDefaults() {
  const ProfileData previous = capture();
  if (apply(defaults())) return true;
  apply(previous);
  return false;
}
