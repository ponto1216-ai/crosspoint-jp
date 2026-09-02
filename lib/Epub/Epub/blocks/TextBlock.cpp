#include "TextBlock.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>
#include <Utf8.h>
#include <VerticalTextUtils.h>

#include "ImageBlock.h"

// U+FFFC (OBJECT REPLACEMENT CHARACTER) — インライン画像のダミー文字（ParsedText の addImage と同一）。
static constexpr const char* INLINE_IMAGE_MARKER = "\xef\xbf\xbc";

#include <algorithm>
#include <climits>
static std::vector<std::string> splitUtf8Chars(const std::string& text) {
  std::vector<std::string> chars;

  const char* p = text.c_str();

  while (*p) {
    const char* start = p;
    const unsigned char c = static_cast<unsigned char>(*p);

    if ((c & 0x80) == 0) {
      p += 1;
    } else if ((c & 0xE0) == 0xC0) {
      p += 2;
    } else if ((c & 0xF0) == 0xE0) {
      p += 3;
    } else {
      p += 4;
    }

    chars.emplace_back(start, p - start);
  }

  return chars;
}
int TextBlock::rubyFontId = 0;

void TextBlock::collectCodepoints(std::vector<uint32_t>& out, size_t max) const {
  if (max == 0 || out.size() >= max) {
    return;
  }

  for (const auto& word : words) {
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
    uint32_t cp;
    while ((cp = utf8NextCodepoint(&ptr))) {
      // Check if already exists (simple linear search, OK for small sets)
      bool exists = false;
      for (uint32_t existing : out) {
        if (existing == cp) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        out.push_back(cp);
        if (out.size() >= max) {
          return;
        }
      }
    }
  }
}

bool TextBlock::hasRuby() const {
  for (const auto& rt : rubyTexts) {
    if (!rt.empty() && !isRubyContinuation(rt)) return true;
  }
  return false;
}

void TextBlock::appendRubyText(std::string& out) const {
  for (const auto& ruby : rubyTexts) {
    if (!ruby.empty() && !isRubyContinuation(ruby)) out += ruby;
  }
}

int TextBlock::getVerticalRubyRightOverflow(const GfxRenderer& renderer, const int bodyFontId,
                                            const int layoutColumnWidth) {
  if (rubyFontId == 0) return 0;

  int bodyColumnWidth = renderer.getTextAdvanceX(bodyFontId, "\xe4\xb8\x80", EpdFontFamily::REGULAR);
  if (bodyColumnWidth <= 0) bodyColumnWidth = renderer.getLineHeight(bodyFontId);
  const int rubyColumnWidth = renderer.getLineHeight(rubyFontId);
  const bool isBizudLikeFont = bodyColumnWidth == 29 && rubyColumnWidth == 17;
  const int gap = isBizudLikeFont ? 2 : 1;
  const int rubyBaseOffset = isBizudLikeFont ? bodyColumnWidth : bodyColumnWidth * 70 / 100;
  const int occupiedRight = rubyBaseOffset + gap + rubyColumnWidth;
  return std::max(0, occupiedRight - layoutColumnWidth);
}

int TextBlock::getHorizontalRubyTopInset(const GfxRenderer& renderer, const int bodyFontId) {
  if (rubyFontId == 0) return 0;

  const int bodyLineHeight = renderer.getLineHeight(bodyFontId);
  const int rubyLineHeight = renderer.getLineHeight(rubyFontId);
  const bool isBizudLikeFont = bodyLineHeight == 29 && rubyLineHeight == 17;
  const int gap = isBizudLikeFont ? 2 : 1;
  const int rubyBaseOffset = isBizudLikeFont ? bodyLineHeight : bodyLineHeight * 70 / 100;
  constexpr int rubyViewportSafety = 2;
  const int naturalRubyY = bodyLineHeight - rubyBaseOffset - rubyLineHeight - gap;
  return std::max(0, rubyViewportSafety - naturalRubyY);
}

void TextBlock::render(GfxRenderer& renderer, const int fontId, const int x, const int y, const int viewportWidth,
                       const int viewportHeight, const int viewportLeft, const int viewportTop, const int rubyOffsetX,
                       const int rubyOffsetY) const {
  // Validate iterator bounds before rendering
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size()) {
    LOG_ERR("TXB", "Render skipped: size mismatch (words=%u, xpos=%u, styles=%u)\n", (uint32_t)words.size(),
            (uint32_t)wordXpos.size(), (uint32_t)wordStyles.size());
    return;
  }

  const int effectiveFontId = (blockStyle.fontId != 0) ? blockStyle.fontId : fontId;
  /*
  // ルビフォントのグリフをプリロード（SDカードフォントの場合）
  if (rubyFontId != 0 && hasRuby() && renderer.isSdCardFont(rubyFontId)) {
    std::string allRuby;
    for (const auto& rt : rubyTexts) {
      if (!rt.empty()) allRuby += rt;
    }
    if (!allRuby.empty()) {
      renderer.ensureSdCardFontReady(rubyFontId, allRuby.c_str());
    }
  }
*/

  // Compute column width once for Sideways/TateChuYoko centering
  int columnWidth = 0;
  if (isVertical) {
    // Use advance of CJK reference character "一" (U+4E00) as column width
    columnWidth = renderer.getTextAdvanceX(effectiveFontId, "\xe4\xb8\x80", EpdFontFamily::REGULAR);
    if (columnWidth <= 0) columnWidth = renderer.getLineHeight(effectiveFontId);
  }

  // ParsedText reserves this before each inline image. The body glyph may
  // occupy more pixels than its horizontal advance, so center alignment alone
  // can place an image inside the previous glyph's visual cell.
  const int inlineImageLeadingGap =
      isVertical ? std::max(3, renderer.getLineHeight(effectiveFontId) - columnWidth + 3) : 0;

  // The bitmap center of a CJK body glyph can differ from half the advance
  // width. Sideways ASCII and symbols must use this same visual center.
  int verticalBodyCenterOffset = 0;
  if (isVertical) {
    int bodyMinX = 0;
    int bodyMaxX = 0;
    renderer.getTextVisibleBoundsX(effectiveFontId, "\xe4\xb8\x80", &bodyMinX, &bodyMaxX,
                                   EpdFontFamily::REGULAR);  // U+4E00
    verticalBodyCenterOffset = (bodyMinX + bodyMaxX) / 2 - columnWidth / 2;
  }

  // Keep annotations in one vertical column from drawing over each other.
  // This adjusts only ruby glyphs; body-text positions remain unchanged.
  int nextVerticalRubyY = INT_MIN;
  size_t imgIdx = 0;  // words 内の画像マーカー出現順 = inlineImages の index
  for (size_t i = 0; i < words.size(); i++) {
    const EpdFontFamily::Style currentStyle = wordStyles[i];
#if DEBUG_RUBY_RENDER
    const char* rubyForLog = "";
    if (i < rubyTexts.size()) {
      rubyForLog = rubyTexts[i].c_str();
    }

    LOG_INF("TXB", "[WORD_RUBY_MAP] i=%u word=%s ruby=%s rubySize=%u wordsSize=%u vertical=%d",
            static_cast<unsigned>(i), words[i].c_str(), rubyForLog, static_cast<unsigned>(rubyTexts.size()),
            static_cast<unsigned>(words.size()), isVertical ? 1 : 0);
#endif
    // インライン画像（sparse）: words[i] が画像マーカー(U+FFFC)のとき、inlineImages の該当要素
    // （マーカー出現順）を ImageBlock として描画する。画像は回転しない（縦書きでも横向きのまま列内に収める）。
    if (words[i] == INLINE_IMAGE_MARKER) {
      const int imgW =
          (imgIdx < inlineImages.size() && inlineImages[imgIdx].width > 0) ? inlineImages[imgIdx].width : 1;
      const int imgH =
          (imgIdx < inlineImages.size() && inlineImages[imgIdx].height > 0) ? inlineImages[imgIdx].height : 1;
      int imgX = x + wordXpos[i];
      int imgY = y;
      if (isVertical && i < wordYpos.size()) {
        // Keep the leading clearance reserved by ParsedText before the image.
        imgY = y + wordYpos[i] + inlineImageLeadingGap;
        // 縦書き: 列セル内に中央配置（画像は回転しない）
        imgX += (columnWidth - imgW) / 2;
      } else {
        // 横書き: 行内で縦中央寄せ
        const int lineHeight = renderer.getLineHeight(effectiveFontId);
        imgY += (lineHeight - imgH) / 2;
      }
      if (imgIdx < inlineImages.size()) {
        ImageBlock ib(inlineImages[imgIdx].imagePath, static_cast<int16_t>(imgW), static_cast<int16_t>(imgH));
        ib.render(renderer, imgX, imgY);
      }
      imgIdx++;
      continue;
    }
    if (isVertical && i < wordYpos.size()) {
      // 縦書きモード: VerticalBehaviorに応じて描画方法を分岐
      const char* w = words[i].c_str();
      const int wx = x + wordXpos[i];
      const int wy = y + wordYpos[i];

      // Classify: replicate the logic from ChapterHtmlSlimParser::flushPartWordBuffer
      const auto* p = reinterpret_cast<const unsigned char*>(w);
      uint32_t firstCp = utf8NextCodepoint(&p);
      const bool isSingleCodepoint = (firstCp != 0 && *p == '\0');

      if (isSingleCodepoint && utf8IsJapaneseVoicingMark(firstCp)) {
        // EPUBs sometimes place a voiced mark after a character that has no
        // precomposed form (for example, 阿゛). In vertical text it belongs in
        // the same cell as that preceding character, not on its own line.
        bool renderedAsOverlay = false;
        if (i > 0) {
          const size_t baseIndex = i - 1;
          const auto* basePtr = reinterpret_cast<const unsigned char*>(words[baseIndex].c_str());
          const uint32_t baseCp = utf8NextCodepoint(&basePtr);
          if (baseCp != 0 && *basePtr == '\0' && !utf8IsJapaneseVoicingMark(baseCp) &&
              VerticalTextUtils::isUprightInVertical(baseCp) && baseIndex < wordYpos.size()) {
            const auto baseStyle = baseIndex < wordStyles.size() ? wordStyles[baseIndex] : currentStyle;
            const auto baseFontStyle = static_cast<EpdFontFamily::Style>(baseStyle & EpdFontFamily::BOLD_ITALIC);
            const int baseAdvance = renderer.getTextAdvanceX(effectiveFontId, words[baseIndex].c_str(), baseFontStyle);
            const int measuredEm = renderer.getTextAdvanceX(effectiveFontId, "\xE4\xB8\x80", baseFontStyle);  // U+4E00
            const int emAdvance = measuredEm > 0 ? measuredEm : renderer.getLineHeight(effectiveFontId);

            // Halfwidth body glyphs are visibly centered in the CJK column
            // below. Carry the same font-specific adjustment into the mark
            // anchor so BIZUD and Noto side bearings do not separate ｼ and ﾞ.
            const bool hasHalfwidthBase = VerticalTextUtils::isHalfwidthKatakana(baseCp);
            int baseCenterOffset = 0;
            if (hasHalfwidthBase) {
              int baseMinX = 0;
              int baseMaxX = 0;
              int bodyMinX = 0;
              int bodyMaxX = 0;
              renderer.getTextVisibleBoundsX(effectiveFontId, words[baseIndex].c_str(), &baseMinX, &baseMaxX,
                                             baseFontStyle);
              renderer.getTextVisibleBoundsX(effectiveFontId, "\xE4\xB8\x80", &bodyMinX, &bodyMaxX,
                                             baseFontStyle);  // U+4E00
              baseCenterOffset = (bodyMinX + bodyMaxX - baseMinX - baseMaxX) / 2;
            }

            // U+3099/U+309A are combining glyphs whose dots sit farther
            // left in their cell. Spacing and halfwidth marks already have
            // their own right-side bearing, so keep those closer to the
            // base character.
            // Preserve the established fullwidth positioning for unusual
            // sequences such as あﾞ / 阿゛. Only a halfwidth base needs the
            // new advance- and side-bearing-aware anchor.
            const int anchorCell = hasHalfwidthBase ? emAdvance : renderer.getLineHeight(effectiveFontId);
            const int markOffset = (firstCp == 0x3099 || firstCp == 0x309A) ? (anchorCell * 3) / 4
                                   : hasHalfwidthBase                       ? (baseAdvance * 2) / 3
                                                                            : anchorCell / 3;
            int markX = x + wordXpos[baseIndex] + baseCenterOffset + markOffset;
            const bool isCombiningVoicingMark = firstCp == 0x3099 || firstCp == 0x309A;
            if (!hasHalfwidthBase && !isCombiningVoicingMark) {
              // U+309B/U+309C and U+FF9E/U+FF9F carry very different left
              // bearings in Noto and BIZUD. Align their visible center with
              // the base glyph's visible right edge instead of sharing a
              // font-independent origin. Combining marks keep the existing
              // placement, which both device screenshots already validate.
              int baseMinX = 0;
              int baseMaxX = 0;
              int markMinX = 0;
              int markMaxX = 0;
              renderer.getTextVisibleBoundsX(effectiveFontId, words[baseIndex].c_str(), &baseMinX, &baseMaxX,
                                             baseFontStyle);
              renderer.getTextVisibleBoundsX(effectiveFontId, w, &markMinX, &markMaxX, baseFontStyle);
              markX = x + wordXpos[baseIndex] + baseMaxX - (markMinX + markMaxX) / 2;
            }
            const int markY = y + wordYpos[baseIndex] - anchorCell / 8;
            renderer.drawText(effectiveFontId, markX, markY, w, true, currentStyle);
            renderedAsOverlay = true;
          }
        }
        if (renderedAsOverlay) continue;

        // A voicing mark without an immediately preceding base character is
        // its own visible character. Center its narrow glyph inside the CJK
        // column instead of using the font's left side bearing as the origin.
        int markMinX = 0;
        int markMaxX = 0;
        int bodyMinX = 0;
        int bodyMaxX = 0;
        renderer.getTextVisibleBoundsX(effectiveFontId, w, &markMinX, &markMaxX, currentStyle);
        renderer.getTextVisibleBoundsX(effectiveFontId, "\xE4\xB8\x80", &bodyMinX, &bodyMaxX, currentStyle);  // U+4E00
        const int centerOffset = (bodyMinX + bodyMaxX - markMinX - markMaxX) / 2;
        renderer.drawTextVertical(effectiveFontId, wx + centerOffset, wy, w, true, currentStyle);
        continue;
      }

      // 縦書きでは「＝」を90度回転する。半角長音符「ｰ」は
      // drawTextVertical() の句読点経路で回転・配置する。
      const bool forceSidewaysSymbol = isSingleCodepoint && firstCp == 0xFF1D;  // ＝ FULLWIDTH EQUALS SIGN

      const bool isSingleCjk =
          isSingleCodepoint && !forceSidewaysSymbol && VerticalTextUtils::isUprightInVertical(firstCp);

      if (isSingleCjk) {
        int uprightX = wx;
        if (VerticalTextUtils::isHalfwidthKatakana(firstCp) ||
            VerticalTextUtils::isEnclosedAlphanumeric(firstCp)) {
          // Narrow upright glyphs can carry uneven side bearings. Align their
          // visible ink with the body CJK glyph, just as TateChuYoko aligns
          // halfwidth digits below.
          int glyphMinX = 0;
          int glyphMaxX = 0;
          int bodyMinX = 0;
          int bodyMaxX = 0;
          renderer.getTextVisibleBoundsX(effectiveFontId, w, &glyphMinX, &glyphMaxX, currentStyle);
          renderer.getTextVisibleBoundsX(effectiveFontId, "\xE4\xB8\x80", &bodyMinX, &bodyMaxX,
                                         currentStyle);  // U+4E00
          uprightX += (bodyMinX + bodyMaxX - glyphMinX - glyphMaxX) / 2;
        }
        // wordYpos already contains the halfwidth glyph advance plus the
        // fullwidth inter-cell spacing. Adding another half-cell inset here
        // shifts the ink into the next item (and separates a following voiced
        // mark from its base kana).
        renderer.drawTextVertical(effectiveFontId, uprightX, wy, w, true, currentStyle);
      } else {
        const auto tateChuYokoKind = VerticalTextUtils::classifyTateChuYoko(w);
        if (tateChuYokoKind != VerticalTextUtils::TateChuYokoKind::None) {
          // Align the actual halfwidth-digits bounds with a fullwidth digit
          // in the same column. This is more reliable than the abstract cell
          // width: some fonts (notably Noto) have a cell center that differs
          // from the visible fullwidth-numeral center.
          int textMinX = 0;
          int textMaxX = 0;
          renderer.getTextVisibleBoundsX(effectiveFontId, w, &textMinX, &textMaxX, currentStyle);
          int fullwidthDigitMinX = 0;
          int fullwidthDigitMaxX = 0;
          renderer.getTextVisibleBoundsX(effectiveFontId, "\xEF\xBC\x90", &fullwidthDigitMinX, &fullwidthDigitMaxX,
                                         currentStyle);  // U+FF10
          const int centerOffset = (fullwidthDigitMinX + fullwidthDigitMaxX - textMinX - textMaxX) / 2;
          renderer.drawText(effectiveFontId, wx + centerOffset, wy, w, true, currentStyle);
        } else {
          // Sideways: draw rotated 90° CW, centered in the column.
          const int vertShift = renderer.getFontAscenderSize(effectiveFontId) / 3;
          renderer.drawTextSideways(effectiveFontId, wx + verticalBodyCenterOffset, wy + vertShift, w, true,
                                    currentStyle, columnWidth);
        }
      }

      if (i < rubyTexts.size() && !rubyTexts[i].empty() && !isRubyContinuation(rubyTexts[i])) {
#if DEBUG_RUBY_RENDER
        LOG_INF("TXB", "[RUBY_RENDER_CHECK] i=%u rubyFontId=%d word=%s ruby=%s isSingleCjk=%d x=%d y=%d",
                static_cast<unsigned>(i), rubyFontId, words[i].c_str(), rubyTexts[i].c_str(), isSingleCjk ? 1 : 0, wx,
                wy);
#endif
      }

      // 縦書きルビ描画
      if (rubyFontId != 0 && i < rubyTexts.size() && !rubyTexts[i].empty() && !isRubyContinuation(rubyTexts[i])) {
#if DEBUG_RUBY_RENDER
        LOG_INF("TXB", "[RUBY_DRAW] i=%u rubyFontId=%d word=%s ruby=%s x=%d y=%d", static_cast<unsigned>(i), rubyFontId,
                words[i].c_str(), rubyTexts[i].c_str(), wx, wy);
#endif
        const int rubyColumnWidth = renderer.getLineHeight(rubyFontId);

        // BIZUD系は列幅が大きく、従来位置でちょうどよい。
        // その他SDフォントは columnWidth のままだと離れすぎるため本文側へ寄せる。
        const bool isBizudLikeFont = (columnWidth == 29 && rubyColumnWidth == 17);

        const int gap = isBizudLikeFont ? 2 : 1;
        const int rubyBaseOffset = isBizudLikeFont ? columnWidth : columnWidth * 70 / 100;

        const int rightBaseX = wx + rubyBaseOffset + gap;
        // Vertical ruby always stays on the standard right side of its base
        // text. The first column may use the reader's right screen margin.
        // If the margin is too narrow, clamp at the physical screen edge; the
        // resulting overlap with the body text is preferable to flipping ruby
        // to the non-standard left side.
        const int minRubyX = 0;
        const int maxRubyX = std::max(minRubyX, renderer.getScreenWidth() - rubyColumnWidth);
        const int rubyX = std::clamp(rightBaseX + rubyOffsetX, minRubyX, maxRubyX);

        const bool rubyIsAsciiWord = VerticalTextUtils::isAsciiAlphabeticWord(rubyTexts[i].c_str());
        const int rubyNaturalHeight =
            rubyIsAsciiWord
                ? renderer.getTextAdvanceX(rubyFontId, rubyTexts[i].c_str(), EpdFontFamily::REGULAR)
                : renderer.getTextAdvanceYVertical(rubyFontId, rubyTexts[i].c_str(), EpdFontFamily::REGULAR);
        const int rubyTextHeight = std::max(rubyColumnWidth, rubyNaturalHeight);
        constexpr int rubyViewportSafety = 2;
        const int minRubyY = viewportHeight > 0 ? viewportTop + rubyViewportSafety : 0;
        const int maxRubyY =
            viewportHeight > 0 ? std::max(minRubyY, viewportTop + viewportHeight - rubyTextHeight - rubyViewportSafety)
                               : INT_MAX;
        int rubyY = std::clamp(wy + rubyOffsetY, minRubyY, maxRubyY);
        if (rubyY < nextVerticalRubyY) {
          rubyY = std::min(nextVerticalRubyY, maxRubyY);
        }
        nextVerticalRubyY = rubyY + rubyTextHeight + 1;

        if (rubyIsAsciiWord) {
          const int rubyShift = renderer.getFontAscenderSize(rubyFontId) / 3;
          renderer.drawTextSideways(rubyFontId, rubyX, rubyY + rubyShift, rubyTexts[i].c_str(), true,
                                    EpdFontFamily::REGULAR, rubyColumnWidth);
        } else {
          renderer.drawTextVertical(rubyFontId, rubyX, rubyY, rubyTexts[i].c_str(), true, EpdFontFamily::REGULAR);
        }
      }
    } else {
      const int wordX = wordXpos[i] + x;
      renderer.drawText(effectiveFontId, wordX, y, words[i].c_str(), true, currentStyle);
      // 横書きルビ描画
      if (rubyFontId != 0 && i < rubyTexts.size() && !rubyTexts[i].empty() && !isRubyContinuation(rubyTexts[i])) {
        size_t rubyBaseEnd = i;
        while (rubyBaseEnd + 1 < rubyTexts.size() && isRubyContinuation(rubyTexts[rubyBaseEnd + 1])) {
          rubyBaseEnd++;
        }

        // Center over the complete <ruby> base, not only its first CJK token.
        const int lastBaseAdvance =
            renderer.getTextAdvanceX(effectiveFontId, words[rubyBaseEnd].c_str(), wordStyles[rubyBaseEnd]);
        const int baseWidth = wordXpos[rubyBaseEnd] - wordXpos[i] + lastBaseAdvance;
        const int rubyWidth = renderer.getTextAdvanceX(rubyFontId, rubyTexts[i].c_str(), EpdFontFamily::REGULAR);
        const int viewportRight = viewportLeft + viewportWidth;
        const int minRubyX = viewportWidth > 0 ? viewportLeft : 0;
        const int maxRubyX = viewportWidth > 0 ? std::max(minRubyX, viewportRight - rubyWidth) : INT_MAX;
        const int rubyX = std::clamp(wordX + (baseWidth - rubyWidth) / 2 + rubyOffsetX, minRubyX, maxRubyX);
        const int bodyLineHeight = renderer.getLineHeight(effectiveFontId);
        const int rubyLineHeight = renderer.getLineHeight(rubyFontId);

        // Mirror the vertical ruby spacing correction onto the horizontal cross-axis.
        // BIZUD-like metrics keep the original separation; other SD fonts move closer to the body text.
        const bool isBizudLikeFont = (bodyLineHeight == 29 && rubyLineHeight == 17);
        const int gap = isBizudLikeFont ? 2 : 1;
        const int rubyBaseOffset = isBizudLikeFont ? bodyLineHeight : bodyLineHeight * 70 / 100;
        constexpr int rubyViewportSafety = 2;
        const int minRubyY = viewportHeight > 0 ? viewportTop + rubyViewportSafety : 0;
        const int maxRubyY =
            viewportHeight > 0 ? std::max(minRubyY, viewportTop + viewportHeight - rubyLineHeight - rubyViewportSafety)
                               : INT_MAX;
        const int rubyY =
            std::clamp(y + bodyLineHeight - rubyBaseOffset - rubyLineHeight - gap + rubyOffsetY, minRubyY, maxRubyY);
        renderer.drawText(rubyFontId, rubyX, rubyY, rubyTexts[i].c_str(), true, EpdFontFamily::REGULAR);
      }

      if ((currentStyle & EpdFontFamily::UNDERLINE) != 0) {
        const std::string& w = words[i];
        const int fullWordWidth = renderer.getTextWidth(effectiveFontId, w.c_str(), currentStyle);
        // y is the top of the text line; add ascender to reach baseline, then offset 2px below
        const int underlineY = y + renderer.getFontAscenderSize(effectiveFontId) + 2;

        int startX = wordX;
        int underlineWidth = fullWordWidth;

        // if word starts with em-space ("\xe2\x80\x83"), account for the additional indent before drawing the line
        if (w.size() >= 3 && static_cast<uint8_t>(w[0]) == 0xE2 && static_cast<uint8_t>(w[1]) == 0x80 &&
            static_cast<uint8_t>(w[2]) == 0x83) {
          const char* visiblePtr = w.c_str() + 3;
          const int prefixWidth = renderer.getTextAdvanceX(effectiveFontId, "\xe2\x80\x83", currentStyle);
          const int visibleWidth = renderer.getTextWidth(effectiveFontId, visiblePtr, currentStyle);
          startX = wordX + prefixWidth;
          underlineWidth = visibleWidth;
        }

        renderer.drawLine(startX, underlineY, startX + underlineWidth, underlineY, true);
      }
    }
  }

  // Draw a rule at the block boundary. In vertical writing the equivalent of
  // an HTML horizontal rule follows the column flow, so it is vertical.
  if (blockStyle.drawSeparatorBelow && viewportWidth > 0) {
    const int lineHeight = renderer.getLineHeight(effectiveFontId);
    if (isVertical) {
      // Only an explicit HTML <hr> becomes a vertical rule. h1/h2 separators
      // are horizontal-writing underlines; turning those into page-height
      // lines makes table-of-contents headings overlap adjacent body columns.
      if (blockStyle.isHtmlRule) {
        const int separatorX = x + lineHeight / 2;
        const int separatorTop = viewportTop + lineHeight / 4;
        const int separatorBottom = viewportTop + viewportHeight - lineHeight / 4;
        if (separatorBottom > separatorTop) {
          renderer.drawLine(separatorX, separatorTop, separatorX, separatorBottom, true);
        }
      }
    } else {
      const int separatorY = y + lineHeight + 2;
      const int separatorStart = x;
      const int separatorEnd = viewportLeft + viewportWidth - blockStyle.rightInset();
      if (separatorEnd > separatorStart) {
        renderer.drawLine(separatorStart, separatorY, separatorEnd, separatorY, true);
      }
    }
  }
}

bool TextBlock::serialize(FsFile& file) const {
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size()) {
    LOG_ERR("TXB", "Serialization failed: size mismatch (words=%u, xpos=%u, styles=%u)\n", words.size(),
            wordXpos.size(), wordStyles.size());
    return false;
  }

  // Word data
  serialization::writePod(file, static_cast<uint16_t>(words.size()));
  for (const auto& w : words) serialization::writeString(file, w);
  for (auto x : wordXpos) serialization::writePod(file, x);
  for (auto s : wordStyles) serialization::writePod(file, s);

  // Style (alignment + margins/padding/indent)
  serialization::writePod(file, blockStyle.alignment);
  serialization::writePod(file, blockStyle.textAlignDefined);
  serialization::writePod(file, blockStyle.marginTop);
  serialization::writePod(file, blockStyle.marginBottom);
  serialization::writePod(file, blockStyle.marginLeft);
  serialization::writePod(file, blockStyle.marginRight);
  serialization::writePod(file, blockStyle.paddingTop);
  serialization::writePod(file, blockStyle.paddingBottom);
  serialization::writePod(file, blockStyle.paddingLeft);
  serialization::writePod(file, blockStyle.paddingRight);
  serialization::writePod(file, blockStyle.textIndent);
  serialization::writePod(file, blockStyle.textIndentDefined);
  serialization::writePod(file, blockStyle.lineHeightMultiplier);
  serialization::writePod(file, blockStyle.fontId);
  serialization::writePod(file, blockStyle.drawSeparatorBelow);
  serialization::writePod(file, blockStyle.isHtmlRule);
  serialization::writePod(file, blockStyle.isListItem);

  // Vertical layout data
  serialization::writePod(file, isVertical);
  if (isVertical) {
    for (auto y : wordYpos) serialization::writePod(file, y);
  }

  // Ruby text data
  for (size_t i = 0; i < words.size(); i++) {
    serialization::writeString(file, (i < rubyTexts.size()) ? rubyTexts[i] : std::string());
  }

  // Inline image data (sparse): 画像の数と内容を書き込む（words 内のマーカー出現順に一致）。
  serialization::writePod(file, static_cast<uint16_t>(inlineImages.size()));
  for (const auto& img : inlineImages) {
    serialization::writeString(file, img.imagePath);
    serialization::writePod(file, img.width);
    serialization::writePod(file, img.height);
  }

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(FsFile& file) {
  uint16_t wc;
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  BlockStyle blockStyle;

  // Word count
  serialization::readPod(file, wc);

  // Sanity check: prevent allocation of unreasonably large vectors (max 10000 words per block)
  if (wc > 10000) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }

  // Word data
  words.resize(wc);
  wordXpos.resize(wc);
  wordStyles.resize(wc);
  for (auto& w : words) serialization::readString(file, w);
  for (auto& x : wordXpos) serialization::readPod(file, x);
  for (auto& s : wordStyles) serialization::readPod(file, s);

  // Style (alignment + margins/padding/indent)
  serialization::readPod(file, blockStyle.alignment);
  serialization::readPod(file, blockStyle.textAlignDefined);
  serialization::readPod(file, blockStyle.marginTop);
  serialization::readPod(file, blockStyle.marginBottom);
  serialization::readPod(file, blockStyle.marginLeft);
  serialization::readPod(file, blockStyle.marginRight);
  serialization::readPod(file, blockStyle.paddingTop);
  serialization::readPod(file, blockStyle.paddingBottom);
  serialization::readPod(file, blockStyle.paddingLeft);
  serialization::readPod(file, blockStyle.paddingRight);
  serialization::readPod(file, blockStyle.textIndent);
  serialization::readPod(file, blockStyle.textIndentDefined);
  serialization::readPod(file, blockStyle.lineHeightMultiplier);
  serialization::readPod(file, blockStyle.fontId);
  serialization::readPod(file, blockStyle.drawSeparatorBelow);
  serialization::readPod(file, blockStyle.isHtmlRule);
  serialization::readPod(file, blockStyle.isListItem);

  // Vertical layout data
  bool vertical = false;
  serialization::readPod(file, vertical);
  std::vector<int16_t> wordYpos;
  if (vertical) {
    wordYpos.resize(wc);
    for (auto& y : wordYpos) serialization::readPod(file, y);
  }

  // Ruby text data
  std::vector<std::string> rubyTexts(wc);
  for (auto& rt : rubyTexts) serialization::readString(file, rt);

  // Inline image data (sparse): 画像の数と内容を読み込む（words 内のマーカー出現順に一致）。
  uint16_t imgCount = 0;
  serialization::readPod(file, imgCount);
  if (imgCount > 10000) {
    LOG_ERR("TXB", "Deserialization failed: inline image count %u exceeds maximum", imgCount);
    return nullptr;
  }
  std::vector<TextBlock::InlineImage> inlineImages;
  inlineImages.reserve(imgCount);
  for (uint16_t i = 0; i < imgCount; i++) {
    TextBlock::InlineImage img;
    serialization::readString(file, img.imagePath);
    serialization::readPod(file, img.width);
    serialization::readPod(file, img.height);
    inlineImages.push_back(std::move(img));
  }

  return std::unique_ptr<TextBlock>(new TextBlock(std::move(words), std::move(wordXpos), std::move(wordStyles),
                                                  blockStyle, std::move(wordYpos), vertical, std::move(rubyTexts),
                                                  std::move(inlineImages)));
}
