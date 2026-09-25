#pragma once
#include <Utf8.h>

#include <vector>

#include "EpdFontFamily.h"
#include "FontCacheManager.h"
class GfxRenderer {
 public:
  std::map<int, EpdFontFamily> fonts{{1, {}}};
  FontCacheManager cache;
  static constexpr int W = 900, H = 160;
  std::vector<uint8_t> pixels = std::vector<uint8_t>(W * H, 255);
  // Diagonal stroke: distinguishes the selected font glyph from a geometric circle.
  uint8_t bitmap[4] = {0x82, 0x08, 0x20, 0x80};
  const auto& getFontMap() const { return fonts; }
  const uint8_t* getGlyphBitmap(const EpdFontData*, const EpdGlyph*) const { return bitmap; }
  FontCacheManager* getFontCacheManager() { return &cache; }
  int bodyHeight = 25, rubyHeight = 12;
  int getLineHeight(int id) const { return id == 2 ? rubyHeight : bodyHeight; }
  struct Draw {
    int font, x, y;
    std::string text;
  };
  std::vector<Draw> draws;
  int getScreenWidth() const { return W; }
  int getFontAscenderSize(int id) const { return getLineHeight(id); }
  int getTextAdvanceYVertical(int id, const char* text, EpdFontFamily::Style style) const {
    return getTextAdvanceX(id, text, style);
  }
  int getTextWidth(int id, const char* text, EpdFontFamily::Style style) const {
    return getTextAdvanceX(id, text, style);
  }
  void getTextVisibleBoundsX(int id, const char* text, int* left, int* right, EpdFontFamily::Style style) const {
    *left = 0;
    *right = getTextAdvanceX(id, text, style);
  }
  void drawText(int id, int x, int y, const char* text, bool, EpdFontFamily::Style) {
    draws.push_back({id, x, y, text});
  }
  void drawTextVertical(int id, int x, int y, const char* text, bool b, EpdFontFamily::Style s) {
    drawText(id, x, y, text, b, s);
  }
  void drawTextSideways(int id, int x, int y, const char* text, bool b, EpdFontFamily::Style s, int) {
    drawText(id, x, y, text, b, s);
  }
  void drawLine(int, int, int, int, bool) {}
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style) const {
    auto* p = reinterpret_cast<const unsigned char*>(text);
    int width = 0;
    while (uint32_t cp = utf8NextCodepoint(&p)) width += cp < 128 ? 10 : bodyHeight;
    return width;
  }
  void drawPixel(int x, int y, bool) {
    if (x >= 0 && x < W && y >= 0 && y < H) pixels[y * W + x] = 0;
  }
};
