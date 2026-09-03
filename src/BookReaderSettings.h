#pragma once

#include <cstdint>

#include "CrossPointSettings.h"

// Persistent, partial reader-setting overrides for one EPUB source.  The
// source fingerprint, rather than a path, keeps an override attached when a
// book is moved or renamed on the SD card.
namespace BookReaderSettings {

enum DirectionField : uint16_t {
  DirectionFont = 1 << 0,
  DirectionFontSize = 1 << 1,
  DirectionLineSpacing = 1 << 2,
  DirectionCharSpacing = 1 << 3,
  DirectionAlignment = 1 << 4,
  DirectionParagraphSpacing = 1 << 5,
  DirectionMargin = 1 << 6,
  DirectionIndent = 1 << 7,
  DirectionRubyEnabled = 1 << 8,
  DirectionRubyOffsetX = 1 << 9,
  DirectionRubyOffsetY = 1 << 10,
};

enum ReaderField : uint16_t {
  WritingMode = 1 << 0,
  Orientation = 1 << 1,
  BookStyle = 1 << 2,
  ImageRendering = 1 << 3,
  InvertImages = 1 << 4,
};

struct DirectionOverride {
  uint16_t fields = 0;
  DirectionSettings values{};
};

struct Override {
  DirectionOverride horizontal;
  DirectionOverride vertical;
  uint16_t fields = 0;
  uint8_t writingMode = CrossPointSettings::WM_AUTO;
  uint8_t orientation = CrossPointSettings::PORTRAIT;
  uint8_t bookStyle = CrossPointSettings::BALANCED_STYLE;
  uint8_t imageRendering = CrossPointSettings::IMAGES_DISPLAY;
  uint8_t invertImages = 0;
};

bool load(uint64_t fingerprint, Override& result);
bool save(uint64_t fingerprint, const Override& value);
bool remove(uint64_t fingerprint);
// Copies a missing override to a new archive fingerprint during a same-path
// EPUB update. The prior override remains available under its original key.
bool migrate(uint64_t previousFingerprint, uint64_t currentFingerprint);
bool hasAnyField(const Override& value);
Override captureAll(const CrossPointSettings& settings);

// Apply an override to an in-memory effective reader configuration.  Fields
// absent from the override deliberately remain unchanged (Global fallback).
void apply(const Override& value, CrossPointSettings& settings);

}  // namespace BookReaderSettings
