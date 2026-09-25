#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Utf8.h>
#include <VerticalTextUtils.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "TextBlock.h"

namespace {
// A mark is at most 8x8 monochrome pixels, independent of the font cache lifetime.
uint64_t circleMark(int size, bool open) {
  uint64_t pixels = 0;
  const int radius = size - 1;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const int dx = 2 * x - radius, dy = 2 * y - radius;
      const int distance = dx * dx + dy * dy;
      if (distance <= radius * radius + 1 && (!open || distance >= (radius - 2) * (radius - 2))) {
        pixels |= uint64_t{1} << (y * 8 + x);
      }
    }
  }
  return pixels;
}

uint64_t fontMark(GfxRenderer& renderer, int fontId, uint32_t cp, int size) {
  const auto font = renderer.getFontMap().find(fontId);
  if (font == renderer.getFontMap().end()) return 0;
  // Exact lookup also supports SD fonts; never accept the missing-glyph box.
  const EpdGlyph* glyph = font->second.getGlyphExact(cp);
  if (!glyph || !glyph->width || !glyph->height) return 0;
  const EpdGlyph metrics = *glyph;
  const auto* data = font->second.getData();
  const uint8_t* bitmap = renderer.getGlyphBitmap(data, glyph);
  if (!bitmap) return 0;
  const auto ink = [&](int x, int y) {
    const int pixel = y * metrics.width + x;
    return data->is2Bit ? ((bitmap[pixel / 4] >> ((3 - pixel % 4) * 2)) & 3) >= 2
                        : ((bitmap[pixel / 8] >> (7 - pixel % 8)) & 1) != 0;
  };
  int left = metrics.width, top = metrics.height, right = -1, bottom = -1;
  for (int y = 0; y < metrics.height; ++y) {
    for (int x = 0; x < metrics.width; ++x) {
      if (ink(x, y)) {
        left = std::min(left, x);
        right = std::max(right, x);
        top = std::min(top, y);
        bottom = std::max(bottom, y);
      }
    }
  }
  if (right < left) return 0;
  const int width = right - left + 1, height = bottom - top + 1;
  const int longest = std::max(width, height);
  const int outWidth = std::max(1, size * width / longest);
  const int outHeight = std::max(1, size * height / longest);
  uint64_t pixels = 0;
  for (int y = 0; y < outHeight; ++y) {
    for (int x = 0; x < outWidth; ++x) {
      int count = 0, total = 0;
      for (int sy = y * height / outHeight; sy < std::max(y * height / outHeight + 1, (y + 1) * height / outHeight);
           ++sy) {
        for (int sx = x * width / outWidth; sx < std::max(x * width / outWidth + 1, (x + 1) * width / outWidth); ++sx) {
          ++total;
          if (ink(left + sx, top + sy)) ++count;
        }
      }
      if (count && count * 3 >= total) {
        pixels |= uint64_t{1} << ((y + (size - outHeight) / 2) * 8 + x + (size - outWidth) / 2);
      }
    }
  }
  return pixels;
}
}  // namespace

bool TextBlock::tokenHasEmphasis(size_t index) const {
  if (index >= words.size() || index >= emphasis.size() || emphasis[index] == TextEmphasis::None ||
      !textEmphasis::valid(static_cast<uint8_t>(emphasis[index])))
    return false;
  auto* p = reinterpret_cast<const unsigned char*>(words[index].c_str());
  while (uint32_t cp = utf8NextCodepoint(&p))
    if (textEmphasis::eligible(cp)) return true;
  return false;
}

bool TextBlock::rubyBaseHasEmphasis(size_t start) const {
  if (start >= words.size() || start >= rubyTexts.size() || rubyTexts[start].empty() ||
      isRubyContinuation(rubyTexts[start]))
    return false;
  // Only the start token and its continuation tokens belong to this ruby.
  size_t i = start;
  do {
    if (tokenHasEmphasis(i)) return true;
    ++i;
  } while (i < words.size() && i < rubyTexts.size() && isRubyContinuation(rubyTexts[i]));
  return false;
}

bool TextBlock::hasEmphasizedRuby() const {
  for (size_t i = 0; i < rubyTexts.size() && i < words.size(); ++i)
    if (rubyBaseHasEmphasis(i)) return true;
  return false;
}

bool TextBlock::hasEmphasis() const {
  for (size_t i = 0; i < emphasis.size() && i < words.size(); ++i) {
    if (tokenHasEmphasis(i)) return true;
  }
  return false;
}

int TextBlock::emphasisSize(const GfxRenderer& renderer, int fontId) {
  return std::clamp(renderer.getLineHeight(fontId) / 5, 4, 8);
}

int TextBlock::annotationRightOverflow(const GfxRenderer& renderer, int fontId, int columnWidth) const {
  const int ruby = hasRuby() ? getVerticalRubyRightOverflow(renderer, fontId, columnWidth) : 0;
  if (!hasEmphasis()) return ruby;
  int bodyWidth = renderer.getTextAdvanceX(fontId, "\xe4\xb8\x80", EpdFontFamily::REGULAR);
  if (bodyWidth <= 0) bodyWidth = renderer.getLineHeight(fontId);
  const int occupied = bodyWidth + 2 + emphasisSize(renderer, fontId) +
                       (rubyFontId != 0 && hasEmphasizedRuby() ? 2 + renderer.getLineHeight(rubyFontId) : 0);
  return std::max(ruby, std::max(0, occupied - columnWidth));
}

int TextBlock::annotationTopInset(const GfxRenderer& renderer, int fontId) const {
  const int ruby = hasRuby() ? getHorizontalRubyTopInset(renderer, fontId) : 0;
  if (!hasEmphasis()) return ruby;
  return std::max(ruby, emphasisSize(renderer, fontId) + 2 +
                            (rubyFontId != 0 && hasEmphasizedRuby() ? 2 + renderer.getLineHeight(rubyFontId) : 0));
}

void TextBlock::renderEmphasis(GfxRenderer& renderer, int fontId, int x, int y) const {
  if (emphasis.empty()) return;
  // The font prewarm pass must not draw pixels into the framebuffer.
  const auto* cache = renderer.getFontCacheManager();
  if (cache && cache->isScanning()) return;
  const int size = emphasisSize(renderer, fontId);
  int bodyWidth = renderer.getTextAdvanceX(fontId, "\xe4\xb8\x80", EpdFontFamily::REGULAR);
  if (bodyWidth <= 0) bodyWidth = renderer.getLineHeight(fontId);
  std::array<uint64_t, 13> marks{};
  uint16_t prepared = 0;
  for (size_t i = 0; i < emphasis.size() && i < words.size(); ++i) {
    const auto value = emphasis[i];
    if (value == TextEmphasis::None || !textEmphasis::valid(static_cast<uint8_t>(value))) continue;
    const size_t markIndex = static_cast<uint8_t>(value);
    if (!(prepared & (1u << markIndex))) {
      const uint32_t cp = textEmphasis::codepoint(value, isVertical);
      marks[markIndex] = fontMark(renderer, fontId, cp, size);
      const uint32_t fallback = textEmphasis::open(value) ? 0x25e6 : 0x2022;
      if (!marks[markIndex] && cp != fallback) marks[markIndex] = fontMark(renderer, fontId, fallback, size);
      if (!marks[markIndex]) marks[markIndex] = circleMark(size, textEmphasis::open(value));
      prepared |= 1u << markIndex;
    }
    const auto draw = [&](int centerX, int centerY) {
      for (int dy = 0; dy < size; ++dy)
        for (int dx = 0; dx < size; ++dx) {
          if (marks[markIndex] & (uint64_t{1} << (dy * 8 + dx))) {
            renderer.drawPixel(centerX - size / 2 + dx, centerY - size / 2 + dy, true);
          }
        }
    };
    const auto* p = reinterpret_cast<const unsigned char*>(words[i].c_str());
    if (isVertical) {
      bool eligible = false;
      while (uint32_t cp = utf8NextCodepoint(&p)) eligible |= textEmphasis::eligible(cp);
      if (!eligible || i >= wordYpos.size()) continue;
      const bool upright = VerticalTextUtils::isUprightInVertical([&]() {
        auto* q = reinterpret_cast<const unsigned char*>(words[i].c_str());
        return utf8NextCodepoint(&q);
      }());
      const bool tcy = VerticalTextUtils::classifyTateChuYoko(words[i].c_str(), tateChuYokoMaxDigits) !=
                       VerticalTextUtils::TateChuYokoKind::None;
      if (upright || tcy) {
        const int advance = renderer.getLineHeight(fontId);
        draw(x + wordXpos[i] + bodyWidth + 2 + size / 2, y + wordYpos[i] + advance / 2);
      } else {
        // A sideways Latin run occupies the column along its horizontal text
        // advance. CSS text-emphasis applies to each eligible character, not
        // to the run as one oversized glyph.
        // Match drawTextSideways(), which shifts the rotated body down by one
        // third of the ascender. Without this, sesame marks sit visibly above
        // the centers of their Latin glyphs.
        const int sidewaysShift = renderer.getFontAscenderSize(fontId) / 3;
        p = reinterpret_cast<const unsigned char*>(words[i].c_str());
        std::string prefix;
        prefix.reserve(words[i].size());
        int runningAdvance = 0;
        const bool useLinearAdvance = renderer.isSdCardFont(fontId);
        while (*p) {
          const auto* start = p;
          const uint32_t cp = utf8NextCodepoint(&p);
          const int before =
              useLinearAdvance ? runningAdvance : renderer.getTextAdvanceX(fontId, prefix.c_str(), wordStyles[i]);
          const std::string character(reinterpret_cast<const char*>(start), p - start);
          prefix.append(character);
          const int after = useLinearAdvance
                                ? before + renderer.getTextAdvanceX(fontId, character.c_str(), wordStyles[i])
                                : renderer.getTextAdvanceX(fontId, prefix.c_str(), wordStyles[i]);
          runningAdvance = after;
          if (textEmphasis::eligible(cp)) {
            draw(x + wordXpos[i] + bodyWidth + 2 + size / 2, y + wordYpos[i] + sidewaysShift + (before + after) / 2);
          }
        }
      }
    } else {
      std::string prefix;
      prefix.reserve(words[i].size());
      int runningAdvance = 0;
      const bool useLinearAdvance = renderer.isSdCardFont(fontId);
      while (*p) {
        const auto* start = p;
        const uint32_t cp = utf8NextCodepoint(&p);
        const int before =
            useLinearAdvance ? runningAdvance : renderer.getTextAdvanceX(fontId, prefix.c_str(), wordStyles[i]);
        const std::string character(reinterpret_cast<const char*>(start), p - start);
        prefix.append(character);
        const int after = useLinearAdvance ? before + renderer.getTextAdvanceX(fontId, character.c_str(), wordStyles[i])
                                           : renderer.getTextAdvanceX(fontId, prefix.c_str(), wordStyles[i]);
        runningAdvance = after;
        if (textEmphasis::eligible(cp)) draw(x + wordXpos[i] + (before + after) / 2, y - 2 - (size + 1) / 2);
      }
    }
  }
}
