#include "BookReaderSettings.h"

#include <ArduinoJson.h>
#include <HalStorage.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr char kSettingsPath[] = "/.crosspoint/book-reader-settings.json";
constexpr uint8_t kFormatVersion = 1;

void fingerprintKey(const uint64_t fingerprint, char (&out)[17]) {
  snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(fingerprint));
}

bool readU8(JsonObjectConst object, const char* key, uint8_t min, uint8_t max, uint8_t& target, uint16_t& mask,
            const uint16_t field) {
  const JsonVariantConst input = object[key];
  if (input.isNull()) return true;
  if (!input.is<int>()) return false;
  const int value = input.as<int>();
  if (value < min || value > max) return false;
  target = static_cast<uint8_t>(value);
  mask |= field;
  return true;
}

void writeDirection(JsonObject object, const BookReaderSettings::DirectionOverride& value) {
  const auto fields = value.fields;
  const auto& settings = value.values;
  if (fields & BookReaderSettings::DirectionFont) {
    object["fontFamily"] = settings.fontFamily;
    object["sdFontFamilyName"] = settings.sdFontFamilyName;
  }
  if (fields & BookReaderSettings::DirectionFontSize) object["fontSize"] = settings.fontSize;
  if (fields & BookReaderSettings::DirectionLineSpacing) object["lineSpacing"] = settings.lineSpacing;
  if (fields & BookReaderSettings::DirectionCharSpacing) object["charSpacing"] = settings.charSpacing;
  if (fields & BookReaderSettings::DirectionAlignment) object["paragraphAlignment"] = settings.paragraphAlignment;
  if (fields & BookReaderSettings::DirectionParagraphSpacing) object["extraParagraphSpacing"] = settings.extraParagraphSpacing;
  if (fields & BookReaderSettings::DirectionMargin) object["screenMargin"] = settings.screenMargin;
  if (fields & BookReaderSettings::DirectionIndent) object["firstLineIndent"] = settings.firstLineIndent;
  if (fields & BookReaderSettings::DirectionRubyEnabled) object["rubyEnabled"] = settings.rubyEnabled;
  if (fields & BookReaderSettings::DirectionRubyOffsetX) object["rubyOffsetX"] = settings.rubyOffsetX;
  if (fields & BookReaderSettings::DirectionRubyOffsetY) object["rubyOffsetY"] = settings.rubyOffsetY;
}

bool readDirection(JsonObjectConst object, BookReaderSettings::DirectionOverride& result) {
  if (object.isNull()) return true;
  auto& settings = result.values;
  auto& fields = result.fields;
  const JsonVariantConst fontFamily = object["fontFamily"];
  const JsonVariantConst familyName = object["sdFontFamilyName"];
  if (!fontFamily.isNull() || !familyName.isNull()) {
    if (!fontFamily.is<int>() || !familyName.is<const char*>()) return false;
    const int family = fontFamily.as<int>();
    const char* name = familyName.as<const char*>();
    if (family < 0 || family >= CrossPointSettings::FONT_FAMILY_COUNT || strlen(name) >= sizeof(settings.sdFontFamilyName)) {
      return false;
    }
    settings.fontFamily = static_cast<uint8_t>(family);
    strncpy(settings.sdFontFamilyName, name, sizeof(settings.sdFontFamilyName) - 1);
    settings.sdFontFamilyName[sizeof(settings.sdFontFamilyName) - 1] = '\0';
    fields |= BookReaderSettings::DirectionFont;
  }
  return readU8(object, "fontSize", 0, CrossPointSettings::FONT_SIZE_COUNT - 1, settings.fontSize, fields,
                BookReaderSettings::DirectionFontSize) &&
         readU8(object, "lineSpacing", CrossPointSettings::LINE_SPACING_MIN, CrossPointSettings::LINE_SPACING_MAX,
                settings.lineSpacing, fields, BookReaderSettings::DirectionLineSpacing) &&
         readU8(object, "charSpacing", 0, 50, settings.charSpacing, fields, BookReaderSettings::DirectionCharSpacing) &&
         readU8(object, "paragraphAlignment", 0, CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT - 1,
                settings.paragraphAlignment, fields, BookReaderSettings::DirectionAlignment) &&
         readU8(object, "extraParagraphSpacing", 0, 4, settings.extraParagraphSpacing, fields,
                BookReaderSettings::DirectionParagraphSpacing) &&
         readU8(object, "screenMargin", 5, 40, settings.screenMargin, fields, BookReaderSettings::DirectionMargin) &&
         readU8(object, "firstLineIndent", 0, 1, settings.firstLineIndent, fields, BookReaderSettings::DirectionIndent) &&
         readU8(object, "rubyEnabled", 0, 1, settings.rubyEnabled, fields,
                BookReaderSettings::DirectionRubyEnabled) &&
         readU8(object, "rubyOffsetX", 0, 80, settings.rubyOffsetX, fields,
                BookReaderSettings::DirectionRubyOffsetX) &&
         readU8(object, "rubyOffsetY", 0, 80, settings.rubyOffsetY, fields,
                BookReaderSettings::DirectionRubyOffsetY);
}

bool readOverride(JsonObjectConst object, BookReaderSettings::Override& result) {
  if (object.isNull()) return false;
  if (!readDirection(object["horizontal"].as<JsonObjectConst>(), result.horizontal) ||
      !readDirection(object["vertical"].as<JsonObjectConst>(), result.vertical)) {
    return false;
  }
  return readU8(object, "writingMode", 0, CrossPointSettings::WRITING_MODE_COUNT - 1, result.writingMode,
                result.fields, BookReaderSettings::WritingMode) &&
         readU8(object, "orientation", 0, CrossPointSettings::ORIENTATION_COUNT - 1, result.orientation,
                result.fields, BookReaderSettings::Orientation) &&
         readU8(object, "bookStyle", 0, CrossPointSettings::BOOK_STYLE_COUNT - 1, result.bookStyle, result.fields,
                BookReaderSettings::BookStyle) &&
         readU8(object, "imageRendering", 0, CrossPointSettings::IMAGE_RENDERING_COUNT - 1, result.imageRendering,
                result.fields, BookReaderSettings::ImageRendering) &&
         readU8(object, "invertImages", 0, 1, result.invertImages, result.fields, BookReaderSettings::InvertImages);
}

void writeOverride(JsonObject object, const BookReaderSettings::Override& value) {
  object.clear();
  if (value.horizontal.fields != 0) writeDirection(object["horizontal"].to<JsonObject>(), value.horizontal);
  if (value.vertical.fields != 0) writeDirection(object["vertical"].to<JsonObject>(), value.vertical);
  if (value.fields & BookReaderSettings::WritingMode) object["writingMode"] = value.writingMode;
  if (value.fields & BookReaderSettings::Orientation) object["orientation"] = value.orientation;
  if (value.fields & BookReaderSettings::BookStyle) object["bookStyle"] = value.bookStyle;
  if (value.fields & BookReaderSettings::ImageRendering) object["imageRendering"] = value.imageRendering;
  if (value.fields & BookReaderSettings::InvertImages) object["invertImages"] = value.invertImages;
}

bool loadDocument(JsonDocument& document) {
  if (!Storage.exists(kSettingsPath)) {
    document["formatVersion"] = kFormatVersion;
    document["books"].to<JsonObject>();
    return true;
  }
  const String json = Storage.readFile(kSettingsPath);
  if (json.isEmpty() || deserializeJson(document, json)) return false;
  return (document["formatVersion"] | 0) == kFormatVersion && document["books"].is<JsonObject>();
}

void applyDirection(const BookReaderSettings::DirectionOverride& value, DirectionSettings& target) {
  const auto fields = value.fields;
  const auto& source = value.values;
  if (fields & BookReaderSettings::DirectionFont) {
    target.fontFamily = source.fontFamily;
    strncpy(target.sdFontFamilyName, source.sdFontFamilyName, sizeof(target.sdFontFamilyName) - 1);
    target.sdFontFamilyName[sizeof(target.sdFontFamilyName) - 1] = '\0';
  }
  if (fields & BookReaderSettings::DirectionFontSize) target.fontSize = source.fontSize;
  if (fields & BookReaderSettings::DirectionLineSpacing) target.lineSpacing = source.lineSpacing;
  if (fields & BookReaderSettings::DirectionCharSpacing) target.charSpacing = source.charSpacing;
  if (fields & BookReaderSettings::DirectionAlignment) target.paragraphAlignment = source.paragraphAlignment;
  if (fields & BookReaderSettings::DirectionParagraphSpacing) target.extraParagraphSpacing = source.extraParagraphSpacing;
  if (fields & BookReaderSettings::DirectionMargin) target.screenMargin = source.screenMargin;
  if (fields & BookReaderSettings::DirectionIndent) target.firstLineIndent = source.firstLineIndent;
  if (fields & BookReaderSettings::DirectionRubyEnabled) target.rubyEnabled = source.rubyEnabled;
  if (fields & BookReaderSettings::DirectionRubyOffsetX) target.rubyOffsetX = source.rubyOffsetX;
  if (fields & BookReaderSettings::DirectionRubyOffsetY) target.rubyOffsetY = source.rubyOffsetY;
}

}  // namespace

bool BookReaderSettings::hasAnyField(const Override& value) {
  return value.fields != 0 || value.horizontal.fields != 0 || value.vertical.fields != 0;
}

BookReaderSettings::Override BookReaderSettings::captureAll(const CrossPointSettings& settings) {
  Override result;
  result.horizontal.fields = DirectionFont | DirectionFontSize | DirectionLineSpacing | DirectionCharSpacing |
                             DirectionAlignment | DirectionParagraphSpacing | DirectionMargin | DirectionIndent |
                             DirectionRubyEnabled | DirectionRubyOffsetX | DirectionRubyOffsetY;
  result.vertical.fields = result.horizontal.fields;
  result.horizontal.values = settings.horizontal;
  result.vertical.values = settings.vertical;
  result.fields = WritingMode | Orientation | BookStyle | ImageRendering | InvertImages;
  result.writingMode = settings.writingMode;
  result.orientation = settings.orientation;
  result.bookStyle = settings.embeddedStyle;
  result.imageRendering = settings.imageRendering;
  result.invertImages = settings.invertImages;
  return result;
}

bool BookReaderSettings::load(const uint64_t fingerprint, Override& result) {
  result = Override{};
  if (!Storage.ready()) return false;
  JsonDocument document;
  if (!loadDocument(document)) return false;
  char key[17];
  fingerprintKey(fingerprint, key);
  const JsonObjectConst entry = document["books"][key].as<JsonObjectConst>();
  if (entry.isNull()) return true;
  return readOverride(entry, result);
}

bool BookReaderSettings::save(const uint64_t fingerprint, const Override& value) {
  if (!Storage.ready() || !Storage.ensureDirectoryExists("/.crosspoint")) return false;
  JsonDocument document;
  if (!loadDocument(document)) return false;
  char key[17];
  fingerprintKey(fingerprint, key);
  JsonObject books = document["books"].as<JsonObject>();
  if (!hasAnyField(value)) {
    books.remove(key);
  } else {
    writeOverride(books[key].to<JsonObject>(), value);
  }
  String json;
  serializeJson(document, json);
  return Storage.writeFile(kSettingsPath, json);
}

bool BookReaderSettings::remove(const uint64_t fingerprint) { return save(fingerprint, Override{}); }

bool BookReaderSettings::migrate(const uint64_t previousFingerprint, const uint64_t currentFingerprint) {
  if (previousFingerprint == currentFingerprint || !Storage.ready() || !Storage.ensureDirectoryExists("/.crosspoint")) {
    return previousFingerprint == currentFingerprint;
  }
  JsonDocument document;
  if (!loadDocument(document)) return false;
  char previousKey[17];
  char currentKey[17];
  fingerprintKey(previousFingerprint, previousKey);
  fingerprintKey(currentFingerprint, currentKey);
  JsonObject books = document["books"].as<JsonObject>();
  if (!books[currentKey].isNull() || books[previousKey].isNull()) return true;
  Override previous;
  if (!readOverride(books[previousKey].as<JsonObjectConst>(), previous)) return false;
  if (!hasAnyField(previous)) return true;
  writeOverride(books[currentKey].to<JsonObject>(), previous);
  String json;
  serializeJson(document, json);
  return Storage.writeFile(kSettingsPath, json);
}

void BookReaderSettings::apply(const Override& value, CrossPointSettings& settings) {
  applyDirection(value.horizontal, settings.horizontal);
  applyDirection(value.vertical, settings.vertical);
  if (value.fields & WritingMode) settings.writingMode = value.writingMode;
  if (value.fields & Orientation) settings.orientation = value.orientation;
  if (value.fields & BookStyle) settings.embeddedStyle = value.bookStyle;
  if (value.fields & ImageRendering) settings.imageRendering = value.imageRendering;
  if (value.fields & InvertImages) settings.invertImages = value.invertImages;
}
