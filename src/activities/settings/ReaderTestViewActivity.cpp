#include "ReaderTestViewActivity.h"

#include <EpdFontFamily.h>
#include <Epub/ParsedText.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"

namespace {

// This uses the reader's layout and ruby renderer, not a UI-font mock-up.
// Keep nine short paragraphs/columns so the result can be compared directly
// with a real EPUB page: ruby and non-ruby rows must coexist on one screen.
ParsedText makeSampleLine(const DirectionSettings& settings, const size_t sampleIndex) {
  struct SampleLine {
    const char* number;
    const char* ruby;
    bool hasRuby;
    const char* suffix;
  };
  static constexpr SampleLine kLines[] = {
      {"一", "だいいちれつ", true, "の本文です。"},
      {"二", "", false, "（ABC 12 123ー）です。"},
      {"三", "", false, "は圏点確認です。"},
      {"四", "だいよんれつ", true, "はルビ・圏点併用です。"},
      {"五", "", false, "は太字確認です。"},
      {"六", "", false, "は通常本文です。"},
      {"七", "だいななれつ", true,
       "の本文です。これは改行位置を確認するための長い文章です。表示設定による行送りと余白も確認します。"},
      {"八", "", false, "はルビなし本文です。"},
      {"九", "", false, "『春夏秋冬』です。"},
  };
  const auto& line = kLines[sampleIndex % (sizeof(kLines) / sizeof(kLines[0]))];
  ParsedText parsed(settings.hyphenationEnabled != 0, BlockStyle{}, settings.firstLineIndent != 0,
                    settings.tateChuYokoMaxDigits);
  const auto add = [&parsed](const char* body) { parsed.addWord(body, EpdFontFamily::REGULAR); };
  const auto addBold = [&parsed](const char* body) { parsed.addWord(body, EpdFontFamily::BOLD); };
  const auto addRuby = [&parsed, &add](const char* first, const char* second, const char* third, const char* ruby) {
    const size_t firstIndex = parsed.size();
    add(first);
    add(second);
    add(third);
    parsed.setRubyForWordAt(firstIndex, ruby, 3);
  };
  if (line.hasRuby) {
    addRuby("第", line.number, "列", line.ruby);
    if (sampleIndex == 3) parsed.setEmphasisFrom(0, TextEmphasis::FilledSesame);
  } else {
    add("第");
    add(line.number);
    add("列");
  }
  // Keep two-digit runs as independent words so vertical layout exercises the
  // same tate-chu-yoko path as EPUB text instead of grouping them with ASCII.
  if (sampleIndex == 1) {
    add("（");
    add("ABC");
    add(" ");
    add("12");
    add(" ");
    add("123");
    add("ー");
    add("）");
    add("で");
    add("す");
    add("。");
    return parsed;
  }
  if (sampleIndex == 2) {
    add("は");
    const size_t emphasisStart = parsed.size();
    add("圏");
    add("点");
    add("確");
    add("認");
    parsed.setEmphasisFrom(emphasisStart, TextEmphasis::FilledSesame);
    add("で");
    add("す");
    add("。");
    return parsed;
  }

  // ParsedText expects separate tokens; this is enough for the sample's CJK
  // layout while retaining the remaining short ASCII runs as one word.
  const char* suffix = line.suffix;
  while (*suffix) {
    const unsigned char c = static_cast<unsigned char>(*suffix);
    const size_t bytes = c < 0x80 ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
    if (c < 0x80 && (c == 'A' || c == '1')) {
      const char* end = suffix;
      while (*end && *end != '\xE3') ++end;
      add(std::string(suffix, static_cast<size_t>(end - suffix)).c_str());
      suffix = end;
    } else {
      add(std::string(suffix, bytes).c_str());
      suffix += bytes;
    }
  }
  if (sampleIndex == 4) {
    add(" ");
    addBold("太");
    addBold("字");
    add("・");
    addBold("Bold");
  }
  return parsed;
}

int rubyOffset(const uint8_t stored) { return static_cast<int>(std::min<uint8_t>(stored, 80)) - 16; }

int resolveRubyFont(const DirectionSettings& settings, const int bodyFontId) {
  if (!settings.rubyEnabled) return 0;
  constexpr uint8_t kRubyFontSize = 5;  // 8pt, matching EpubReaderActivity.
  if (settings.sdFontFamilyName[0] != '\0' && SETTINGS.sdFontIdResolver) {
    const int rubyFontId =
        SETTINGS.sdFontIdResolver(SETTINGS.sdFontResolverCtx, settings.sdFontFamilyName, kRubyFontSize);
    if (rubyFontId != 0) return rubyFontId;
  }
  return bodyFontId;
}

}  // namespace

void ReaderTestViewActivity::onEnter() {
  Activity::onEnter();
  vertical = initialVertical >= 0 ? initialVertical != 0 : SETTINGS.writingMode == CrossPointSettings::WM_VERTICAL;
  // SettingsActivity opens this screen with Confirm. Do not treat that
  // button release as a request to enter ruby adjustment.
  ignoreOpeningConfirmRelease = true;
  requestUpdate();
}

void ReaderTestViewActivity::loop() {
  if (ignoreOpeningConfirmRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) ignoreOpeningConfirmRelease = false;
    return;
  }
  if (rubyAdjustActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      rubyAdjustActive = false;
      persistRubyAdjust();
      rubyAdjustChanged = false;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) adjustRubyOffset(true, -1);
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) adjustRubyOffset(true, 1);
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) adjustRubyOffset(false, -1);
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) adjustRubyOffset(false, 1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && vertical) {
    vertical = false;
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) && !vertical) {
    vertical = true;
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    rubyAdjustActive = true;
    rubyAdjustChanged = false;
    requestUpdate();
  }
}

void ReaderTestViewActivity::persistRubyAdjust() {
  if (!rubyAdjustChanged) return;
  if (bookFingerprint == 0) {
    SETTINGS.saveToFile();
    return;
  }

  BookReaderSettings::Override value;
  if (!BookReaderSettings::load(bookFingerprint, value)) {
    LOG_ERR("BOOKSET", "Could not load book settings for test view");
    return;
  }
  const auto current = BookReaderSettings::captureAll(SETTINGS);
  auto& target = vertical ? value.vertical : value.horizontal;
  const auto& source = vertical ? current.vertical : current.horizontal;
  target.values.rubyEnabled = source.values.rubyEnabled;
  target.values.rubyOffsetX = source.values.rubyOffsetX;
  target.values.rubyOffsetY = source.values.rubyOffsetY;
  target.fields |= BookReaderSettings::DirectionRubyEnabled | BookReaderSettings::DirectionRubyOffsetX |
                   BookReaderSettings::DirectionRubyOffsetY;
  if (!BookReaderSettings::save(bookFingerprint, value)) {
    LOG_ERR("BOOKSET", "Could not save ruby settings from test view");
  }
}

void ReaderTestViewActivity::adjustRubyOffset(const bool xAxis, const int delta) {
  auto& settings = SETTINGS.getDirectionSettings(vertical);
  uint8_t& target = xAxis ? settings.rubyOffsetX : settings.rubyOffsetY;
  const int next = std::clamp(static_cast<int>(target) + delta, 0, 80);
  if (next == target) return;
  target = static_cast<uint8_t>(next);
  rubyAdjustChanged = true;
  requestUpdate();
}

void ReaderTestViewActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto layout = UiLayout::from(renderer);
  int contentX = layout.content.x;
  int contentY = layout.content.y;
  int availableWidth = layout.content.width;
  int availableHeight = layout.content.height;
  // Reserve the ruby-adjust strips before the controls appear so horizontal
  // text keeps the same wrapping and position while adjustment is opened.
  if (layout.landscape) {
    if (gpio.deviceIsX3()) {
      constexpr int sideHintGutter = 54;
      availableWidth -= sideHintGutter;
      if (!layout.frontHintsOnLeft) contentX += sideHintGutter;
    } else {
      // X4 side keys are shown along a horizontal edge in landscape. Match
      // Settings instead of consuming the X3-only 54 px vertical strip.
      const int sideHintGutter = metrics.sideButtonHintsWidth;
      availableHeight -= sideHintGutter;
      if (renderer.getOrientation() == GfxRenderer::Orientation::LandscapeCounterClockwise) {
        contentY += sideHintGutter;
      }
    }
  } else {
    // Portrait X3 places one side-button hint on each edge; X4 stacks both
    // on the right. Keep the preview and its annotations out of those strips.
    const int sideHintGutter = metrics.sideButtonHintsWidth;
    availableWidth -= sideHintGutter;
    if (gpio.deviceIsX3()) {
      contentX += sideHintGutter;
      availableWidth -= sideHintGutter;
    }
  }
  char headerValue[24] = {};
  if (rubyAdjustActive) {
    const auto& settings = SETTINGS.getDirectionSettings(vertical);
    snprintf(headerValue, sizeof(headerValue), "X:%+d Y:%+d", rubyOffset(settings.rubyOffsetX),
             rubyOffset(settings.rubyOffsetY));
  }
  GUI.drawHeader(renderer, Rect{contentX, contentY + metrics.topPadding, availableWidth, metrics.headerHeight},
                 tr(STR_READER_TEST_VIEW),
                 rubyAdjustActive ? headerValue : (vertical ? tr(STR_WM_VERTICAL) : tr(STR_WM_HORIZONTAL)));

  const int top = contentY + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottom = contentY + availableHeight -
                     (layout.landscape ? metrics.verticalSpacing : metrics.buttonHintsHeight + metrics.verticalSpacing);
  const int left = contentX + metrics.contentSidePadding;
  const int contentWidth = availableWidth - metrics.contentSidePadding * 2;
  const int contentHeight = bottom - top;
  // Loading a reader from the browser normally does this before resolving its
  // font ID. The test view can switch direction in place, so repeat it here.
  ensureSdFontLoaded(vertical);
  // Match normal EPUB rendering: optional three-digit TateChuYoko uses the
  // smaller companion font so all three digits fit one vertical body cell.
  configureSmallFont(vertical);
  const auto& direction = SETTINGS.getDirectionSettings(vertical);
  const int fontId = SETTINGS.getReaderFontId(vertical);
  if (fontId != 0 && contentWidth > 0 && contentHeight > 0) {
    const int rubyFontId = resolveRubyFont(direction, fontId);
    if (vertical) {
      renderer.ensureSdCardVerticalGlyphsReady(fontId);
    }
    if (auto* cache = renderer.getFontCacheManager()) {
      // The 8pt ruby ID and the body ID are virtual sizes of the same SD
      // font.  Calling prewarm twice would let the ruby pass replace the
      // body's compact glyph cache, forcing every body glyph back through
      // the slow on-demand SD path.  Prewarm both sets together once.
      constexpr const char* kPreviewRegularGlyphs =
          "第一列の本文です。第二列（ABC 12 123ー）です。第三列は圏点確認です。"
          "第四列はルビ・圏点併用です。第五列は太字確認です。第六列は通常本文です。"
          "第七列の本文です。これは改行位置を確認するための長い文章です。"
          "表示設定による行送りと余白も確認します。第八列はルビなし本文です。第九列『春夏秋冬』です。"
          "だいいちれつだいよんれつだいななれつ";
      cache->prewarmCache(fontId, kPreviewRegularGlyphs, 0x01);
      // Bold occurs only in the dedicated verification sample, so keep its
      // prewarm small without sacrificing its first-render correctness.
      cache->prewarmCache(fontId, "太字Bold", 0x02);
    }
    TextBlock::rubyFontId = rubyFontId;
    const int offsetX = rubyOffset(direction.rubyOffsetX) + (vertical ? 0 : 10);
    const int offsetY = rubyOffset(direction.rubyOffsetY);
    if (vertical) {
      renderer.setVerticalCharSpacing(direction.charSpacing);
      // Mirror ChapterHtmlSlimParser::addLineToPage(): in vertical writing,
      // the reader's line-spacing setting scales the column width, then adds
      // the normal quarter-column gutter between adjacent columns.
      const int columnWidth =
          std::max(1, static_cast<int>(renderer.getLineHeight(fontId) * SETTINGS.getReaderLineCompression(true)));
      const int columnSpacing = columnWidth / 4;
      int nextColumnX = left + contentWidth - columnWidth;
      bool firstColumn = true;
      for (size_t sampleIndex = 0; sampleIndex < 9 && nextColumnX >= left; ++sampleIndex) {
        ParsedText sample = makeSampleLine(direction, sampleIndex);
        std::vector<std::shared_ptr<TextBlock>> columns;
        sample.layoutVerticalColumns(renderer, fontId, static_cast<uint16_t>(contentHeight),
                                     [&columns](std::shared_ptr<TextBlock> column) {
                                       columns.push_back(std::move(column));
                                       return true;
                                     });
        for (const auto& column : columns) {
          int annotationRightInset = 0;
          if (column->hasRuby() || column->hasEmphasis()) {
            const int overflow = column->annotationRightOverflow(renderer, fontId, columnWidth);
            // Match normal EPUB layout: the first column clears the page edge,
            // while later columns reuse the configured inter-column spacing.
            annotationRightInset = firstColumn ? overflow : std::max(0, overflow - columnSpacing);
          }
          const int columnX = nextColumnX - annotationRightInset;
          if (columnX < left) break;
          column->render(renderer, fontId, columnX, top, contentWidth, contentHeight, left, top, offsetX, offsetY);
          firstColumn = false;
          nextColumnX = columnX - (columnWidth + columnSpacing);
        }
      }
    } else {
      const int bodyLineHeight = renderer.getLineHeight(fontId);
      // Keep the preview's baseline advance and paragraph gap on the exact
      // settings path used by ChapterHtmlSlimParser.  A sample item represents
      // one EPUB <p>, so omitting extraParagraphSpacing made horizontal text
      // look much tighter than the same text in the reader.
      const int lineAdvance = std::max(1, static_cast<int>(bodyLineHeight * SETTINGS.getReaderLineCompression(false)));
      const int paragraphGap = (lineAdvance * static_cast<int>(direction.extraParagraphSpacing)) / 6;
      int y = top;
      for (size_t sampleIndex = 0; sampleIndex < 9; ++sampleIndex) {
        ParsedText sample = makeSampleLine(direction, sampleIndex);
        std::vector<std::shared_ptr<TextBlock>> lines;
        sample.layoutAndExtractLines(renderer, fontId, static_cast<uint16_t>(contentWidth),
                                     [&lines](std::shared_ptr<TextBlock> line) {
                                       lines.push_back(std::move(line));
                                       return true;
                                     });
        for (const auto& line : lines) {
          if (line->hasRuby() || line->hasEmphasis()) {
            const int annotationInset = line->annotationTopInset(renderer, fontId);
            // The configured line spacing already creates part of this clearance.
            // Add only the missing amount, exactly as normal EPUB page layout does.
            const int existingLeading = std::max(0, lineAdvance - bodyLineHeight);
            y += (y == top) ? annotationInset : std::max(0, annotationInset - existingLeading);
          }
          if (y + bodyLineHeight > bottom) break;
          line->render(renderer, fontId, left, y, contentWidth, contentHeight, left, top, offsetX, offsetY);
          y += lineAdvance;
        }
        if (y + bodyLineHeight > bottom) break;
        // Match ChapterHtmlSlimParser::makePages(), which applies the reader
        // setting after each paragraph rather than after every wrapped line.
        if (sampleIndex + 1 < 9) y += paragraphGap;
      }
    }
  }
  const auto labels =
      rubyAdjustActive
          ? mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), tr(STR_RUBY_X_MINUS), tr(STR_RUBY_X_PLUS))
          : mappedInput.mapLabels(tr(STR_BACK), tr(STR_RUBY_OFFSET), tr(STR_WM_HORIZONTAL), tr(STR_WM_VERTICAL));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (rubyAdjustActive) {
    GUI.drawSideButtonHints(renderer, tr(STR_RUBY_Y_MINUS), tr(STR_RUBY_Y_PLUS));
  }
  renderer.displayBuffer();
}
