#pragma once
#include <Arduino.h>
#include <EpdFontFamily.h>
#include <Utf8.h>
#include <VerticalTextUtils.h>
class GfxRenderer {
 public:
  struct MeasureOnlyScope {
    explicit MeasureOnlyScope(const GfxRenderer&) {}
  };
  int getFontAscenderSize(int id) const { return getLineHeight(id); }
  void getTextVisibleBoundsX(int id, const char* text, int* left, int* right, EpdFontFamily::Style style) const {
    *left = 0;
    *right = getTextAdvanceX(id, text, style) - 1;
  }
  bool sd = false;
  bool exhaustPrewarm = false;
  bool narrowEllipsis = false;
  bool narrowHalfwidth = false;
  bool tallLineHeight = false;
  const char* fontPath = nullptr;
  int getEllipsisDigitTightening(int, int cjkAdvance) const {
    return VerticalTextUtils::ellipsisDigitTightening(fontPath, cjkAdvance);
  }
  bool isSdCardFont(int) const { return sd; }
  void ensureSdCardFontReady(int, const char*, uint8_t) const {
    if (exhaustPrewarm) ESP.free = ESP.largest = 512;
  }
  int getLineHeight(int) const { return tallLineHeight ? 42 : 20; }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 5; }
  int getVerticalCharSpacing() const { return 0; }
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style) const {
    auto* p = reinterpret_cast<const unsigned char*>(text);
    int width = 0;
    while (const uint32_t cp = utf8NextCodepoint(&p)) {
      width += cp == 0x00AD || cp == 0x200B     ? 0
               : cp == ' '                      ? 5
               : narrowEllipsis && (cp == 0x2025 || cp == 0x2026) ? 10
               : narrowHalfwidth && VerticalTextUtils::isHalfwidthKatakana(cp) ? 10
               : cp < 128                       ? 10
                                                : tallLineHeight ? 29 : 20;
    }
    return width;
  }
  int getTextWidth(int font, const char* text, EpdFontFamily::Style style) const {
    return getTextAdvanceX(font, text, style);
  }
};
