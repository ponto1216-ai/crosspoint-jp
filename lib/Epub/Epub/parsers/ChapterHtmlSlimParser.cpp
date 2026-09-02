#include "ChapterHtmlSlimParser.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>
#include <VerticalTextUtils.h>
#include <expat.h>

#include "../../Epub.h"
#include "../Page.h"
#include "../blocks/TableRowBlock.h"
#include "../blocks/TextBlock.h"
#include "../converters/ImageDecoderFactory.h"
#include "../converters/ImageToFramebufferDecoder.h"
#include "../htmlEntities.h"

const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr int NUM_HEADER_TAGS = sizeof(HEADER_TAGS) / sizeof(HEADER_TAGS[0]);

// Minimum file size (in bytes) to show indexing popup - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_POPUP = 10 * 1024;  // 10KB
constexpr size_t PARSE_BUFFER_SIZE = 1024;
// Minimum free heap to continue parsing. Below this, stop gracefully
// to prevent abort() from failed allocations (no C++ exceptions on ESP32).
constexpr size_t MIN_FREE_HEAP_FOR_PARSING = 20 * 1024;  // 20KB
// ParsedText reserves 800 word slots. Check before each normal word so one
// 1KB Expat callback cannot grow a vector past that reservation before its
// end-of-callback flush runs.
constexpr size_t TEXT_BLOCK_SAFE_WORD_LIMIT = 700;
constexpr uint8_t MAX_CONSECUTIVE_EXPLICIT_BLANK_LINES = 1;

const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote"};
constexpr int NUM_BLOCK_TAGS = sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]);

const char* BOLD_TAGS[] = {"b", "strong"};
constexpr int NUM_BOLD_TAGS = sizeof(BOLD_TAGS) / sizeof(BOLD_TAGS[0]);

const char* ITALIC_TAGS[] = {"i", "em"};
constexpr int NUM_ITALIC_TAGS = sizeof(ITALIC_TAGS) / sizeof(ITALIC_TAGS[0]);

const char* UNDERLINE_TAGS[] = {"u", "ins"};
constexpr int NUM_UNDERLINE_TAGS = sizeof(UNDERLINE_TAGS) / sizeof(UNDERLINE_TAGS[0]);

// EPUB fixed-layout covers commonly use SVG <image xlink:href="..."> rather
// than HTML <img src="...">.  Both ultimately refer to a raster item in the
// EPUB and can share the normal image extraction/rendering path.
const char* IMAGE_TAGS[] = {"img", "image"};
constexpr int NUM_IMAGE_TAGS = sizeof(IMAGE_TAGS) / sizeof(IMAGE_TAGS[0]);

constexpr float MIN_CSS_FONT_SCALE = 0.75f;
constexpr float MAX_CSS_FONT_SCALE = 1.50f;

float cssFontScale(const CssLength& size, const float currentEmSize) {
  switch (size.unit) {
    case CssUnit::Percent:
      return size.value / 100.0f;
    case CssUnit::Em:
    case CssUnit::Rem:
      return size.value;
    default:
      return size.toPixels(currentEmSize) / std::max(1.0f, currentEmSize);
  }
}

const char* SKIP_TAGS[] = {"head", "style", "script", "title", "rp"};
constexpr int NUM_SKIP_TAGS = sizeof(SKIP_TAGS) / sizeof(SKIP_TAGS[0]);

// Balanced mode takes paragraph geometry and semantic inline emphasis from the
// book. Reader-controlled typography, writing direction, visibility and image
// dimensions stay under CrossPoint control; headings are handled separately.
void retainBalancedParagraphStyle(CssStyle& style) {
  CssStyle balanced;
  if (style.hasFontStyle()) {
    balanced.fontStyle = style.fontStyle;
    balanced.defined.fontStyle = 1;
  }
  if (style.hasFontWeight()) {
    balanced.fontWeight = style.fontWeight;
    balanced.defined.fontWeight = 1;
  }
  if (style.hasTextDecoration()) {
    balanced.textDecoration = style.textDecoration;
    balanced.defined.textDecoration = 1;
  }
  if (style.hasTextAlign()) {
    balanced.textAlign = style.textAlign;
    balanced.defined.textAlign = 1;
  }
  if (style.hasTextIndent()) {
    balanced.textIndent = style.textIndent;
    balanced.defined.textIndent = 1;
  }
  if (style.hasMarginTop()) {
    balanced.marginTop = style.marginTop;
    balanced.defined.marginTop = 1;
  }
  if (style.hasMarginBottom()) {
    balanced.marginBottom = style.marginBottom;
    balanced.defined.marginBottom = 1;
  }
  if (style.hasMarginLeft()) {
    balanced.marginLeft = style.marginLeft;
    balanced.defined.marginLeft = 1;
  }
  if (style.hasMarginRight()) {
    balanced.marginRight = style.marginRight;
    balanced.defined.marginRight = 1;
  }
  if (style.hasPaddingTop()) {
    balanced.paddingTop = style.paddingTop;
    balanced.defined.paddingTop = 1;
  }
  if (style.hasPaddingBottom()) {
    balanced.paddingBottom = style.paddingBottom;
    balanced.defined.paddingBottom = 1;
  }
  if (style.hasPaddingLeft()) {
    balanced.paddingLeft = style.paddingLeft;
    balanced.defined.paddingLeft = 1;
  }
  if (style.hasPaddingRight()) {
    balanced.paddingRight = style.paddingRight;
    balanced.defined.paddingRight = 1;
  }
  style = balanced;
}

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

bool hasClassToken(const std::string& classAttr, const char* token) {
  const size_t tokenLength = strlen(token);
  size_t pos = 0;
  while (pos < classAttr.size()) {
    while (pos < classAttr.size() && isWhitespace(classAttr[pos])) ++pos;
    const size_t start = pos;
    while (pos < classAttr.size() && !isWhitespace(classAttr[pos])) ++pos;
    if (pos - start == tokenLength && classAttr.compare(start, tokenLength, token) == 0) return true;
  }
  return false;
}

// Check if a Unicode codepoint is an invisible/zero-width character that should be skipped
bool isInvisibleCodepoint(const uint32_t cp) {
  if (cp == 0xFEFF) return true;                  // BOM / Zero Width No-Break Space
  if (cp == 0x200B) return true;                  // Zero Width Space
  if (cp == 0x200C) return true;                  // Zero Width Non-Joiner
  if (cp == 0x200D) return true;                  // Zero Width Joiner
  if (cp == 0x200E) return true;                  // Left-to-Right Mark
  if (cp == 0x200F) return true;                  // Right-to-Left Mark
  if (cp == 0x2060) return true;                  // Word Joiner
  if (cp == 0x00AD) return true;                  // Soft Hyphen
  if (cp == 0x034F) return true;                  // Combining Grapheme Joiner
  if (cp == 0x061C) return true;                  // Arabic Letter Mark
  if (cp >= 0x2066 && cp <= 0x2069) return true;  // Directional isolates
  if (cp >= 0x202A && cp <= 0x202E) return true;  // Directional formatting
  return false;
}

// Check if a Unicode codepoint is CJK (Chinese/Japanese/Korean)
// CJK characters should be treated as individual "words" for line breaking
bool isCjkCodepointForSplit(const uint32_t cp) {
  // CJK Unified Ideographs: U+4E00 - U+9FFF
  if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
  // CJK Unified Ideographs Extension A: U+3400 - U+4DBF
  if (cp >= 0x3400 && cp <= 0x4DBF) return true;
  // CJK Unified Ideographs Extensions B-F: supplementary-plane ideographs
  // such as U+20B9F 𠮟 must remain individual vertical cells, rather than
  // joining an ASCII-style sideways run.
  if (cp >= 0x20000 && cp <= 0x2EBEF) return true;
  // CJK Punctuation: U+3000 - U+303F
  if (cp >= 0x3000 && cp <= 0x303F) return true;
  // Hiragana: U+3040 - U+309F
  if (cp >= 0x3040 && cp <= 0x309F) return true;
  // Katakana: U+30A0 - U+30FF
  if (cp >= 0x30A0 && cp <= 0x30FF) return true;
  // Circled digits and letters must stay individual upright cells in vertical
  // text, rather than joining the surrounding sideways text run.
  if (VerticalTextUtils::isEnclosedAlphanumeric(cp)) return true;
  // CJK Compatibility Ideographs: U+F900 - U+FAFF
  if (cp >= 0xF900 && cp <= 0xFAFF) return true;
  // Fullwidth forms: U+FF00 - U+FFEF
  if (cp >= 0xFF00 && cp <= 0xFFEF) return true;
  // Horizontal bars use dedicated vertical glyphs and must occupy one cell
  // each instead of being merged into a sideways text run.
  if (cp == 0x2014 || cp == 0x2015) return true;
  // Symbols that remain upright in Japanese vertical text. Splitting them
  // prevents a trailing symbol from being joined to an ASCII run (e.g. 1△).
  if (cp == 0x2605 || cp == 0x2606 ||  // ★ ☆
      cp == 0x25BD || cp == 0x25BC ||  // ▽ ▼
      cp == 0x25B3 || cp == 0x25B2 ||  // △ ▲
      cp == 0x2642 || cp == 0x2640 ||  // ♂ ♀
      cp == 0x266A)                    // ♪
    return true;
  return false;
}

// Get UTF-8 byte length for a lead byte
int getUtf8ByteLength(unsigned char leadByte) {
  if ((leadByte & 0x80) == 0) return 1;     // ASCII: 0xxxxxxx
  if ((leadByte & 0xE0) == 0xC0) return 2;  // 2-byte: 110xxxxx
  if ((leadByte & 0xF0) == 0xE0) return 3;  // 3-byte: 1110xxxx
  if ((leadByte & 0xF8) == 0xF0) return 4;  // 4-byte: 11110xxx
  return 1;                                 // Invalid, treat as single byte
}

// Decode UTF-8 codepoint from bytes
uint32_t decodeUtf8Codepoint(const char* s, int len) {
  if (len <= 0) return 0;
  unsigned char b0 = static_cast<unsigned char>(s[0]);

  if ((b0 & 0x80) == 0) {
    return b0;  // ASCII
  }
  if (len >= 2 && (b0 & 0xE0) == 0xC0) {
    unsigned char b1 = static_cast<unsigned char>(s[1]);
    return ((b0 & 0x1F) << 6) | (b1 & 0x3F);
  }
  if (len >= 3 && (b0 & 0xF0) == 0xE0) {
    unsigned char b1 = static_cast<unsigned char>(s[1]);
    unsigned char b2 = static_cast<unsigned char>(s[2]);
    return ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
  }
  if (len >= 4 && (b0 & 0xF8) == 0xF0) {
    unsigned char b1 = static_cast<unsigned char>(s[1]);
    unsigned char b2 = static_cast<unsigned char>(s[2]);
    unsigned char b3 = static_cast<unsigned char>(s[3]);
    return ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
  }
  return b0;  // Fallback
}

// given the start and end of a tag, check to see if it matches a known tag
bool matches(const char* tag_name, const char* possible_tags[], const int possible_tag_count) {
  for (int i = 0; i < possible_tag_count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

const char* getAttribute(const XML_Char** atts, const char* attrName) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], attrName) == 0) return atts[i + 1];
  }
  return nullptr;
}

bool isInternalEpubLink(const char* href) {
  if (!href || href[0] == '\0') return false;
  if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) return false;
  if (strncmp(href, "mailto:", 7) == 0) return false;
  if (strncmp(href, "ftp://", 6) == 0) return false;
  if (strncmp(href, "tel:", 4) == 0) return false;
  if (strncmp(href, "javascript:", 11) == 0) return false;
  return true;
}

bool isHeaderOrBlock(const char* name) {
  return matches(name, HEADER_TAGS, NUM_HEADER_TAGS) || matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS);
}

bool isTableStructuralTag(const char* name) {
  return strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 || strcmp(name, "td") == 0 || strcmp(name, "th") == 0;
}

// Update effective bold/italic/underline based on block style and inline style stack
void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  // Start with block-level styles
  effectiveBold = currentCssStyle.hasFontWeight() && currentCssStyle.fontWeight == CssFontWeight::Bold;
  effectiveItalic = currentCssStyle.hasFontStyle() && currentCssStyle.fontStyle == CssFontStyle::Italic;
  effectiveUnderline =
      currentCssStyle.hasTextDecoration() && currentCssStyle.textDecoration == CssTextDecoration::Underline;

  // Apply inline style stack in order
  for (const auto& entry : inlineStyleStack) {
    if (entry.hasBold) {
      effectiveBold = entry.bold;
    }
    if (entry.hasItalic) {
      effectiveItalic = entry.italic;
    }
    if (entry.hasUnderline) {
      effectiveUnderline = entry.underline;
    }
  }
}

void ChapterHtmlSlimParser::noteEmptyBlockContent() {
  for (auto& candidate : emptyBlockCandidates) candidate.hasContent = true;
  consecutiveExplicitBlankLines = 0;
}

void ChapterHtmlSlimParser::noteEmptyBlockBreak() {
  if (emptyBlockCandidates.empty()) return;
  for (size_t i = 0; i + 1 < emptyBlockCandidates.size(); ++i) {
    emptyBlockCandidates[i].hasContent = true;
  }
  emptyBlockCandidates.back().hasExplicitBreak = true;
}

bool ChapterHtmlSlimParser::addExplicitBlankLine() {
  if (!currentTextBlock || consecutiveExplicitBlankLines >= MAX_CONSECUTIVE_EXPLICIT_BLANK_LINES) return false;

  // ParsedText discards empty words. A zero-width space keeps one invisible
  // layout line in the cache while occupying no visible text width.
  currentTextBlock->addWord("\xE2\x80\x8B", EpdFontFamily::REGULAR);
  consecutiveExplicitBlankLines++;
  return true;
}

bool ChapterHtmlSlimParser::consumeEmptyBlockCandidate(const int candidateDepth) {
  if (emptyBlockCandidates.empty() || emptyBlockCandidates.back().depth != candidateDepth) return false;
  const auto candidate = emptyBlockCandidates.back();
  emptyBlockCandidates.pop_back();
  return !candidate.hasContent && !candidate.hasExplicitBreak;
}

// flush the contents of partWordBuffer to currentTextBlock
void ChapterHtmlSlimParser::flushPartWordBuffer() {
  // Determine font style from depth-based tracking and CSS effective style
  const bool isBold = boldUntilDepth < depth || effectiveBold;
  const bool isItalic = italicUntilDepth < depth || effectiveItalic;
  const bool isUnderline = underlineUntilDepth < depth || effectiveUnderline;

  // Combine style flags using bitwise OR
  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (isBold) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD);
  }
  if (isItalic) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::ITALIC);
  }
  if (isUnderline) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::UNDERLINE);
  }

  const bool hasBufferedWord = partWordBufferIndex > 0;
  // flush the buffer
  ensureTextBlockCapacityForWord();
  partWordBuffer[partWordBufferIndex] = '\0';
  if (verticalMode) {
    // Classify short numbers and paired !/? consistently with rendering.
    const auto tateChuYokoKind = VerticalTextUtils::classifyTateChuYoko(partWordBuffer);
    auto vb = VerticalTextUtils::VerticalBehavior::Sideways;  // default for Latin text
    if (tateChuYokoKind != VerticalTextUtils::TateChuYokoKind::None) {
      vb = VerticalTextUtils::VerticalBehavior::TateChuYoko;
    }
    currentTextBlock->addWord(partWordBuffer, fontStyle, vb, false, nextWordContinues);
  } else {
    currentTextBlock->addWord(partWordBuffer, fontStyle, false, nextWordContinues);
  }
  if (hasBufferedWord) noteEmptyBlockContent();
  partWordBufferIndex = 0;
  nextWordContinues = false;
}

void ChapterHtmlSlimParser::flushPendingVerticalWhitespace() {
  if (!pendingVerticalWhitespace) return;
  pendingVerticalWhitespace = false;

  // A leading run has no preceding inline content, so CSS whitespace
  // collapsing leaves it invisible. The same is true for a trailing run,
  // because this helper is only called when another word follows.
  if (!verticalMode || !currentTextBlock || currentTextBlock->isEmpty()) return;

  partWordBuffer[0] = ' ';
  partWordBuffer[1] = '\0';
  partWordBufferIndex = 1;
  flushPartWordBuffer();
}

void ChapterHtmlSlimParser::flushTextBlockForMemory() {
  if (!currentTextBlock || currentTextBlock->isEmpty()) return;

  LOG_DBG("EHP", "Text block approaching word capacity, splitting into multiple pages");
  if (verticalMode) {
    // Preserve the trailing partial column so the next input chunk continues it.
    currentTextBlock->layoutVerticalColumns(
        renderer, fontId, viewportHeight,
        [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); }, false);
  } else {
    const int horizontalInset = currentTextBlock->getBlockStyle().totalHorizontalInset();
    const uint16_t effectiveWidth =
        (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
    currentTextBlock->layoutAndExtractLines(
        renderer, fontId, effectiveWidth,
        [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); }, false);
  }
}

void ChapterHtmlSlimParser::ensureTextBlockCapacityForWord() {
  // A ruby base must remain intact until its <rt> has been applied. The ruby
  // start tag flushes preceding prose before inRuby becomes true.
  if (!inRuby && currentTextBlock && currentTextBlock->size() >= TEXT_BLOCK_SAFE_WORD_LIMIT) {
    flushTextBlockForMemory();
  }
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  nextWordContinues = false;  // New block = new paragraph, no continuation
  pendingVerticalWhitespace = false;
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      // Merge with existing block style to accumulate CSS styling from parent block elements.
      // This handles cases like <div style="margin-bottom:2em"><h1>text</h1></div> where the
      // div's margin should be preserved, even though it has no direct text content.
      currentTextBlock->setBlockStyle(currentTextBlock->getBlockStyle().getCombinedBlockStyle(blockStyle));

      if (!pendingAnchorId.empty()) {
        anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
        pendingAnchorId.clear();
      }
      return;
    }

    makePages();
  }
  // Record deferred anchor after previous block is flushed
  if (!pendingAnchorId.empty()) {
    anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
    pendingAnchorId.clear();
  }
  currentTextBlock.reset(new ParsedText(hyphenationEnabled, blockStyle, firstLineIndent));
  wordsExtractedInBlock = 0;
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  // Metadata and executable/style content must never enter the book text.
  // Include style/script/title separately so malformed EPUBs with these tags
  // outside <head> are still handled safely.
  if (matches(name, SKIP_TAGS, NUM_SKIP_TAGS)) {
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Extract class, style, and id attributes
  std::string classAttr;
  std::string styleAttr;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classAttr = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        styleAttr = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        // Defer recording until startNewTextBlock, after previous block is flushed to pages
        self->pendingAnchorId = atts[i + 1];
      }
    }
  }

  auto centeredBlockStyle = BlockStyle();
  centeredBlockStyle.textAlignDefined = true;
  centeredBlockStyle.alignment = CssTextAlign::Center;

  // Compute CSS style for this element early so display:none can short-circuit
  // before tag-specific branches emit any content or metadata.
  CssStyle cssStyle;
  if (self->cssParser) {
    cssStyle = self->cssParser->resolveStyle(name, classAttr);
    if (!styleAttr.empty()) {
      CssStyle inlineStyle = CssParser::parseInlineStyle(styleAttr);
      cssStyle.applyOver(inlineStyle);
    }
    if (self->bookStyle == 2 && !matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) {
      retainBalancedParagraphStyle(cssStyle);
    }
  }

  // Skip elements with display:none before all fast paths (tables, links, etc.).
  if (cssStyle.hasDisplay() && cssStyle.display == CssDisplay::None) {
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Special handling for tables: buffer cell data for grid rendering.
  if (strcmp(name, "table") == 0) {
    self->noteEmptyBlockContent();
    if (self->tableDepth > 0) {
      self->tableDepth += 1;
      self->depth += 1;
      return;
    }

    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
    }
    self->tableDepth += 1;
    self->tableRowIndex = 0;
    self->tableColIndex = 0;
    self->tableBuffer.clear();
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    self->tableRowIndex += 1;
    self->tableColIndex = 0;
    self->tableBuffer.emplace_back();
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    self->tableColIndex += 1;
    self->tableCellTextBuffer.clear();
    self->tableCellIsHeader = (strcmp(name, "th") == 0);
    self->depth += 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS)) {
    self->noteEmptyBlockContent();
    std::string src;
    std::string svgHref;
    std::string alt;
    if (atts != nullptr) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0) {
          src = atts[i + 1];
        } else if (strcmp(atts[i], "href") == 0 || strcmp(atts[i], "xlink:href") == 0) {
          // SVG 1.1 uses xlink:href and SVG 2 uses href.  Prefer a real
          // HTML src attribute if a malformed document supplies both.
          svgHref = atts[i + 1];
        } else if (strcmp(atts[i], "alt") == 0) {
          alt = atts[i + 1];
        }
      }
      if (src.empty()) src = svgHref;

      // imageRendering: 0=display, 1=placeholder (alt text only), 2=suppress entirely
      if (self->imageRendering == 2) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }

      // Skip image if CSS display:none
      if (self->cssParser && self->bookStyle == 1) {
        CssStyle imgDisplayStyle = self->cssParser->resolveStyle("img", classAttr);
        if (!styleAttr.empty()) {
          imgDisplayStyle.applyOver(CssParser::parseInlineStyle(styleAttr));
        }
        if (imgDisplayStyle.hasDisplay() && imgDisplayStyle.display == CssDisplay::None) {
          self->skipUntilDepth = self->depth;
          self->depth += 1;
          return;
        }
      }

      if (!src.empty() && self->imageRendering != 1) {
        LOG_DBG("EHP", "Found image: src=%s", src.c_str());

        {
          // Resolve the image path relative to the HTML file
          std::string resolvedPath = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->contentBase + src));

          // If format is not directly supported (e.g. SVG), try same-name fallback
          // with supported extensions (.png, .jpg, .jpeg)
          if (!ImageDecoderFactory::isFormatSupported(resolvedPath)) {
            const size_t dotPos = resolvedPath.rfind('.');
            if (dotPos != std::string::npos) {
              const std::string baseName = resolvedPath.substr(0, dotPos);
              static const char* fallbackExts[] = {".png", ".jpg", ".jpeg"};
              for (const auto* ext : fallbackExts) {
                const std::string candidate = baseName + ext;
                size_t candidateSize = 0;
                if (self->epub->getItemSize(candidate, &candidateSize) && candidateSize > 0) {
                  LOG_DBG("EHP", "Using fallback image: %s -> %s", resolvedPath.c_str(), candidate.c_str());
                  resolvedPath = candidate;
                  break;
                }
              }
            }
          }

          if (ImageDecoderFactory::isFormatSupported(resolvedPath)) {
            // Create a unique filename for the cached image
            std::string ext;
            size_t extPos = resolvedPath.rfind('.');
            if (extPos != std::string::npos) {
              ext = resolvedPath.substr(extPos);
            }
            std::string cachedImagePath = self->imageBasePath + std::to_string(self->imageCounter++) + ext;

            // Extract image to cache file
            FsFile cachedImageFile;
            bool extractSuccess = false;
            if (Storage.openFileForWrite("EHP", cachedImagePath, cachedImageFile)) {
              // Images are extracted lazily. A larger transfer buffer cuts SD
              // read/write calls for image-heavy EPUBs without retaining it.
              extractSuccess = self->epub->readItemContentsToStream(resolvedPath, cachedImageFile, 8192);
              cachedImageFile.flush();
              cachedImageFile.close();
            }

            if (extractSuccess) {
              // Avoid stalling every image on normal SD cards. Slow cards may
              // still need a short sync window, so retry dimensions on demand.
              ImageDimensions dims = {0, 0};
              ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedImagePath);
              bool gotDimensions = false;
              for (int attempt = 0; attempt < 3 && !gotDimensions; attempt++) {
                if (attempt > 0) {
                  delay(50);
                }
                gotDimensions = decoder && decoder->getDimensions(cachedImagePath, dims);
              }
              if (gotDimensions) {
                LOG_DBG("EHP", "Image dimensions: %dx%d", dims.width, dims.height);

                int displayWidth = 0;
                int displayHeight = 0;
                const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
                CssStyle imgStyle = (self->cssParser && self->bookStyle == 1)
                                        ? self->cssParser->resolveStyle("img", classAttr)
                                        : CssStyle{};
                // Merge inline style (e.g. style="height: 2em") so it overrides stylesheet rules
                if (!styleAttr.empty() && self->bookStyle == 1) {
                  imgStyle.applyOver(CssParser::parseInlineStyle(styleAttr));
                }
                const bool hasCssHeight = imgStyle.hasImageHeight();
                const bool hasCssWidth = imgStyle.hasImageWidth();

                // Compute effective container width for percentage-based image sizes.
                // If the image is inside a block with horizontal margins/padding (e.g.
                // <div style="margin: 1em 40%">), percentage widths like width:100%
                // should resolve against the container width, not the full viewport.
                int containerWidth = self->viewportWidth;
                if (self->currentTextBlock) {
                  const int inset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
                  if (inset > 0 && inset < self->viewportWidth) {
                    containerWidth = self->viewportWidth - inset;
                  }
                }

                if (hasCssHeight && hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Both CSS height and width set: resolve both as max bounds, then fit image
                  // within those bounds preserving the original aspect ratio. Image decoders use
                  // a single scale factor for both axes; non-uniform scaling causes diagonal distortion.
                  int maxW =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  int maxH = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);

                  if (maxW > containerWidth) maxW = containerWidth;
                  if (maxH > self->viewportHeight) maxH = self->viewportHeight;
                  if (maxW < 1) maxW = 1;
                  if (maxH < 1) maxH = 1;

                  float scaleX = static_cast<float>(maxW) / dims.width;
                  float scaleY = static_cast<float>(maxH) / dims.height;
                  float scale = (scaleX < scaleY) ? scaleX : scaleY;

                  displayWidth = static_cast<int>(dims.width * scale + 0.5f);
                  displayHeight = static_cast<int>(dims.height * scale + 0.5f);

                  if (displayWidth < 1) displayWidth = 1;
                  if (displayHeight < 1) displayHeight = 1;

                  LOG_DBG("EHP", "Display size from CSS height+width: %dx%d", displayWidth, displayHeight);
                } else if (hasCssHeight && !hasCssWidth && dims.width > 0 && dims.height > 0) {
                  // Use CSS height (resolve % against viewport height) and derive width from aspect ratio
                  displayHeight = static_cast<int>(
                      imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                  if (displayHeight < 1) displayHeight = 1;
                  displayWidth =
                      static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayWidth > containerWidth) {
                    displayWidth = containerWidth;
                    // Rescale height to preserve aspect ratio when width is clamped
                    displayHeight =
                        static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                    if (displayHeight < 1) displayHeight = 1;
                  }
                  if (displayWidth < 1) displayWidth = 1;
                  LOG_DBG("EHP", "Display size from CSS height: %dx%d", displayWidth, displayHeight);
                } else if (hasCssWidth && !hasCssHeight && dims.width > 0 && dims.height > 0) {
                  // Use CSS width (resolve % against container width) and derive height from aspect ratio
                  displayWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                  if (displayWidth > containerWidth) displayWidth = containerWidth;
                  if (displayWidth < 1) displayWidth = 1;
                  displayHeight =
                      static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                  if (displayHeight > self->viewportHeight) {
                    displayHeight = self->viewportHeight;
                    // Rescale width to preserve aspect ratio when height is clamped
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayWidth < 1) displayWidth = 1;
                  }
                  if (displayHeight < 1) displayHeight = 1;
                  LOG_DBG("EHP", "Display size from CSS width: %dx%d", displayWidth, displayHeight);
                } else {
                  // Scale to fit container while maintaining aspect ratio
                  int maxWidth = containerWidth;
                  int maxHeight = self->viewportHeight;
                  float scaleX = (dims.width > maxWidth) ? (float)maxWidth / dims.width : 1.0f;
                  float scaleY = (dims.height > maxHeight) ? (float)maxHeight / dims.height : 1.0f;
                  float scale = (scaleX < scaleY) ? scaleX : scaleY;
                  if (scale > 1.0f) scale = 1.0f;

                  displayWidth = (int)(dims.width * scale);
                  displayHeight = (int)(dims.height * scale);
                  LOG_DBG("EHP", "Display size: %dx%d (scale %.2f)", displayWidth, displayHeight, scale);
                }

                // Flush any pending text block so it appears before the image
                if (self->partWordBufferIndex > 0) {
                  self->flushPartWordBuffer();
                }

                // インライン画像（文字の代替）判定: CSS指定の表示サイズが文字セル1つ分以内なら、
                // ブロック図版（専用ページ化）ではなく本文中の文字（Word）として扱う。
                // bookStyle 1(書籍優先)で有効。imgStyle が解決されるのは bookStyle==1 のときのみ。
                if (self->bookStyle == 1 && self->currentTextBlock && (hasCssHeight || hasCssWidth) &&
                    !hasClassToken(classAttr, "fit")) {
                  // 縦書きの比較基準は「文字セル1つ分（フォントサイズ=emSize）」を使う。
                  // 実グリフ幅(一)基準はフォントファミリ依存（サンセリフはグリフが狭い）で、
                  // CSS 1em相当の画像が溢れてブロック図版化してしまうため、フォント非依存に揃える。
                  const bool fitsInline = self->verticalMode
                                              ? (displayWidth <= static_cast<int>(emSize))
                                              : (displayHeight <= self->renderer.getLineHeight(self->fontId));
                  if (fitsInline) {
                    self->currentTextBlock->addImage(cachedImagePath, displayWidth, displayHeight);
                    self->depth += 1;
                    return;
                  }
                }

                if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
                  const BlockStyle parentBlockStyle = self->currentTextBlock->getBlockStyle();
                  self->startNewTextBlock(parentBlockStyle);
                }

                if (self->verticalMode && self->currentPage && !self->currentPage->elements.empty()) {
                  self->completeCurrentPage();
                  self->currentPage.reset(new Page());
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create image page");
                    return;
                  }
                  self->currentPageNextY = 0;
                  self->currentPageNextX = self->viewportWidth - self->renderer.getLineHeight(self->fontId);
                }
                // Japanese EPUB authoring profiles commonly use class="fit" for a
                // full-page illustration. Isolate it in horizontal mode as well so
                // following text cannot occupy the image region.
                const bool pageFitImage = hasClassToken(classAttr, "fit");

                // Create page for image - break if it is page-fit or won't fit.
                if (self->currentPage && !self->currentPage->elements.empty() &&
                    (pageFitImage || self->currentPageNextY + displayHeight > self->viewportHeight)) {
                  self->completeCurrentPage();
                  self->currentPage.reset(new Page());
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create new page");
                    return;
                  }
                  self->currentPageNextY = 0;
                } else if (!self->currentPage) {
                  self->currentPage.reset(new Page());
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create initial page");
                    return;
                  }
                  self->currentPageNextY = 0;
                }

                // Create ImageBlock and add to page
                auto imageBlock = std::make_shared<ImageBlock>(cachedImagePath, displayWidth, displayHeight);
                if (!imageBlock) {
                  LOG_ERR("EHP", "Failed to create ImageBlock");
                  return;
                }
                int xPos = (self->viewportWidth - displayWidth) / 2;
                const int imageY =
                    pageFitImage ? std::max(0, (self->viewportHeight - displayHeight) / 2) : self->currentPageNextY;
                auto pageImage = std::make_shared<PageImage>(imageBlock, xPos, imageY);
                if (!pageImage) {
                  LOG_ERR("EHP", "Failed to create PageImage");
                  return;
                }
                self->currentPage->elements.push_back(pageImage);

                if (self->verticalMode || pageFitImage) {
                  // Vertical images and page-fit illustrations use their own page
                  // to prevent later text from overlapping the image region.
                  self->completeCurrentPage();
                  self->currentPage.reset(new Page());
                  if (!self->currentPage) {
                    LOG_ERR("EHP", "Failed to create page after image");
                    return;
                  }
                  self->currentPageNextY = 0;
                  self->currentPageNextX = self->viewportWidth - self->renderer.getLineHeight(self->fontId);
                } else {
                  self->currentPageNextY += displayHeight;
                }

                self->depth += 1;
                return;
              } else {
                LOG_ERR("EHP", "Failed to get image dimensions");
                Storage.remove(cachedImagePath.c_str());
              }
            } else {
              if (Storage.exists(cachedImagePath.c_str())) {
                if (Storage.remove(cachedImagePath.c_str())) {
                  LOG_DBG("EHP", "Removed failed image extraction: %s", cachedImagePath.c_str());
                } else {
                  LOG_ERR("EHP", "Failed to remove failed image extraction: %s", cachedImagePath.c_str());
                }
              }
              LOG_ERR("EHP", "Failed to extract image: %s", resolvedPath.c_str());
            }
          }  // isFormatSupported
        }
      }

      // Fallback to alt text or placeholder if image processing fails
      {
        std::string placeholder = alt.empty() ? "[Image]" : "[Image: " + alt + "]";
        self->startNewTextBlock(centeredBlockStyle);
        self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
        self->depth += 1;
        self->characterData(userData, placeholder.c_str(), placeholder.length());
        // Skip any child content (skip until parent as we pre-advanced depth above)
        self->skipUntilDepth = self->depth - 1;
        return;
      }
    }
  }

  // Ruby tag handling
  if (strcmp(name, "ruby") == 0) {
    self->flushPartWordBuffer();
    // Ruby has a dedicated parsing path, so it does not reach the generic
    // inline-element branch below. Keep semantic CSS emphasis active while
    // the base text is emitted as individual words.
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration()) {
      StyleStackEntry entry;
      entry.depth = self->depth;
      if (cssStyle.hasFontWeight()) {
        entry.hasBold = true;
        entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
      }
      if (cssStyle.hasFontStyle()) {
        entry.hasItalic = true;
        entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
      }
      if (cssStyle.hasTextDecoration()) {
        entry.hasUnderline = true;
        entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
      }
      entry.rubyTagStyle = true;
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    }
    self->ensureTextBlockCapacityForWord();
    self->inRuby = true;
    self->rubyStartWordIndex = self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0;
    self->rubyTextBuffer.clear();
    self->collectingRubyText = false;
    self->depth += 1;
    return;
  }

  if (strcmp(name, "rb") == 0) {
    // <rb> is ruby base text. Treat it as normal text inside <ruby>.
    self->flushPartWordBuffer();
    // Like <ruby>, <rb> bypasses generic inline style handling. This lets an
    // EPUB style either the complete ruby base or one part of a multi-rb base.
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration()) {
      StyleStackEntry entry;
      entry.depth = self->depth;
      if (cssStyle.hasFontWeight()) {
        entry.hasBold = true;
        entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
      }
      if (cssStyle.hasFontStyle()) {
        entry.hasItalic = true;
        entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
      }
      if (cssStyle.hasTextDecoration()) {
        entry.hasUnderline = true;
        entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
      }
      entry.rubyBaseTagStyle = true;
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    }
    self->depth += 1;
    return;
  }

  if (strcmp(name, "rt") == 0) {
    // <rt> is ruby annotation text.
    self->flushPartWordBuffer();
    self->collectingRubyText = true;
    self->depth += 1;
    return;
  }

  if (strcmp(name, "rp") == 0) {
    // <rp> is fallback punctuation such as （ ）.
    // Ignore its contents.
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Skip blocks with role="doc-pagebreak" and epub:type="pagebreak"
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0 ||
          strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }

  // Detect internal <a href="..."> links (footnotes, cross-references)
  // Note: <aside epub:type="footnote"> elements are rendered as normal content
  // without special handling. Links pointing to them are collected as footnotes.
  if (strcmp(name, "a") == 0) {
    const char* href = getAttribute(atts, "href");

    bool isInternalLink = isInternalEpubLink(href);

    // Special case: javascript:void(0) links with data attributes
    // Example: <a href="javascript:void(0)"
    // data-xyz="{&quot;name&quot;:&quot;OPS/ch2.xhtml&quot;,&quot;frag&quot;:&quot;id46&quot;}">
    if (href && strncmp(href, "javascript:", 11) == 0) {
      isInternalLink = false;
      // TODO: Parse data-* attributes to extract actual href
    }

    if (isInternalLink) {
      // Flush buffer before style change
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      self->insideFootnoteLink = true;
      self->footnoteLinkDepth = self->depth;
      strncpy(self->currentFootnoteLinkHref, href, sizeof(self->currentFootnoteLinkHref) - 1);
      self->currentFootnoteLinkHref[sizeof(self->currentFootnoteLinkHref) - 1] = '\0';
      self->currentFootnoteLinkText[0] = '\0';
      self->currentFootnoteLinkTextLen = 0;

      // Apply underline style to visually indicate the link
      self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
      StyleStackEntry entry;
      entry.depth = self->depth;
      entry.hasUnderline = true;
      entry.underline = true;
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();

      // Skip CSS resolution — we already handled styling for this <a> tag
      self->depth += 1;
      return;
    }
  }

  const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
  // Balanced and Book Priority use an EPUB text-align when it is present.
  // CrossPoint Priority always keeps the reader's configured alignment.
  const bool preferCssAlignment = self->bookStyle != 0;
  const auto userAlignmentBlockStyle = BlockStyle::fromCssStyle(
      cssStyle, emSize, static_cast<CssTextAlign>(self->paragraphAlignment), preferCssAlignment, self->viewportWidth);

  auto bodyBlockStyle = userAlignmentBlockStyle;
  if (self->bookStyle == 1 && cssStyle.hasFontSize()) {
    const float targetEmSize =
        emSize * std::clamp(cssFontScale(cssStyle.fontSize, emSize), MIN_CSS_FONT_SCALE, MAX_CSS_FONT_SCALE);
    int closestFontId = self->fontId;
    float closestDistance =
        std::abs(static_cast<float>(self->renderer.getFontAscenderSize(closestFontId)) - targetEmSize);
    for (const int candidateFontId : self->cssBodyFontIds) {
      if (candidateFontId == 0) continue;
      const float candidateDistance =
          std::abs(static_cast<float>(self->renderer.getFontAscenderSize(candidateFontId)) - targetEmSize);
      if (candidateDistance < closestDistance) {
        closestFontId = candidateFontId;
        closestDistance = candidateDistance;
      }
    }
    bodyBlockStyle.fontId = closestFontId == self->fontId ? 0 : closestFontId;
  }

  if (strcmp(name, "hr") == 0) {
    self->noteEmptyBlockContent();
    // <hr> is a block-level rule. Flush surrounding text first, then use an
    // otherwise empty TextBlock so pagination and cached rendering follow the
    // normal text path in both writing directions.
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
    }

    auto ruleBlockStyle = bodyBlockStyle;
    const int ruleSpacing = self->renderer.getLineHeight(self->fontId) / 3;
    if (!cssStyle.hasMarginTop()) ruleBlockStyle.marginTop = ruleSpacing;
    if (!cssStyle.hasMarginBottom()) ruleBlockStyle.marginBottom = ruleSpacing;
    ruleBlockStyle.textIndent = 0;
    ruleBlockStyle.textIndentDefined = true;
    ruleBlockStyle.drawSeparatorBelow = true;
    ruleBlockStyle.isHtmlRule = true;

    self->currentTextBlock.reset();
    self->startNewTextBlock(ruleBlockStyle);
    // ParsedText deliberately ignores an empty word. A zero-width space keeps
    // this as a real layout line while remaining invisible to the reader.
    self->currentTextBlock->addWord("\xE2\x80\x8B", EpdFontFamily::REGULAR);
    self->makePages();
    self->currentTextBlock.reset();
    // XHTML permits text directly after a self-closing <hr/>. Start a fresh
    // body block now so the following character-data callback never writes
    // through a null currentTextBlock.
    self->startNewTextBlock(bodyBlockStyle);
  } else if (matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) {
    self->currentCssStyle = cssStyle;
    auto headerBlockStyle = BlockStyle::fromCssStyle(
        cssStyle, emSize, static_cast<CssTextAlign>(self->paragraphAlignment), preferCssAlignment, self->viewportWidth);

    // Heading level (h1→1, h2→2, etc.)
    const int level = name[1] - '0';

    // Per-heading fontId override
    if (level >= 1 && level <= 6) {
      headerBlockStyle.fontId = self->headingFontIds[level - 1];
    }

    // Default margins when CSS does not specify them
    const int bodyLineHeight = self->renderer.getLineHeight(self->fontId);
    if (!cssStyle.hasMarginTop()) {
      headerBlockStyle.marginTop = static_cast<int16_t>(bodyLineHeight * 3 / 4);
    }
    if (!cssStyle.hasMarginBottom()) {
      headerBlockStyle.marginBottom = static_cast<int16_t>(bodyLineHeight / 4);
    }

    // Separator line below h1/h2
    if (level <= 2) {
      headerBlockStyle.drawSeparatorBelow = true;
    }

    self->startNewTextBlock(headerBlockStyle);
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS)) {
    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
      }
      const BlockStyle blockStyle = self->currentTextBlock->getBlockStyle();
      if (self->currentTextBlock->isEmpty()) {
        // Keep author-intended empty paragraphs and blank <br/> runs, but
        // collapse the second and later consecutive empty lines.
        self->noteEmptyBlockBreak();
        self->addExplicitBlankLine();
      }
      self->startNewTextBlock(blockStyle);
    } else {
      self->currentCssStyle = cssStyle;

      if (strcmp(name, "li") == 0) {
        // Force a fresh text block for <li> — the "merge with empty block" path
        // in startNewTextBlock can lose our hanging indent settings.
        if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
          self->currentTextBlock.reset();
        }
        auto liBlockStyle = bodyBlockStyle;
        liBlockStyle.isListItem = true;
        const int bulletW = self->renderer.getTextAdvanceX(self->fontId, "\xe2\x80\xa2", EpdFontFamily::REGULAR);
        const int spaceW = self->renderer.getTextAdvanceX(self->fontId, " ", EpdFontFamily::REGULAR);
        // Ensure a visually meaningful indent (at least half line height)
        const int minIndent = self->renderer.getLineHeight(self->fontId) / 2;
        const auto hangIndent = static_cast<int16_t>(std::max(bulletW + spaceW, minIndent));
        liBlockStyle.paddingLeft = static_cast<int16_t>(liBlockStyle.paddingLeft + hangIndent);
        liBlockStyle.textIndent = static_cast<int16_t>(-hangIndent);
        liBlockStyle.textIndentDefined = true;
        self->startNewTextBlock(liBlockStyle);
        self->updateEffectiveInlineStyle();
        self->currentTextBlock->addWord("\xe2\x80\xa2", EpdFontFamily::REGULAR);
        self->noteEmptyBlockContent();
      } else {
        self->startNewTextBlock(bodyBlockStyle);
        self->updateEffectiveInlineStyle();
        if (strcmp(name, "p") == 0 || strcmp(name, "div") == 0) {
          self->emptyBlockCandidates.push_back({self->depth});
        }
      }
    }
  } else if (matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
    // Push inline style entry for underline tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasUnderline = true;
    entry.underline = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BOLD_TAGS, NUM_BOLD_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    // Push inline style entry for bold tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasBold = true;
    entry.bold = true;
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    if (cssStyle.hasTextDecoration()) {
      entry.hasUnderline = true;
      entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS)) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    // Push inline style entry for italic tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasItalic = true;
    entry.italic = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasTextDecoration()) {
      entry.hasUnderline = true;
      entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
    }
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "span") == 0 || !isHeaderOrBlock(name)) {
    // Handle span and other inline elements for CSS styling
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration()) {
      // Flush buffer before style change so preceding text gets current style
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      StyleStackEntry entry;
      entry.depth = self->depth;  // Track depth for matching pop
      if (cssStyle.hasFontWeight()) {
        entry.hasBold = true;
        entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
      }
      if (cssStyle.hasFontStyle()) {
        entry.hasItalic = true;
        entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
      }
      if (cssStyle.hasTextDecoration()) {
        entry.hasUnderline = true;
        entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
      }
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    }
  }

  // Unprocessed tag, just increasing depth and continue forward
  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Skip content of nested table
  if (self->tableDepth > 1) {
    return;
  }

  // Buffer text for table cell (top-level table)
  if (self->tableDepth == 1) {
    for (int i = 0; i < len; i++) {
      char c = s[i];
      if (c == '\n' || c == '\r' || c == '\t') c = ' ';
      if (c == ' ' && (self->tableCellTextBuffer.empty() || self->tableCellTextBuffer.back() == ' ')) {
        continue;
      }
      self->tableCellTextBuffer += c;
    }
    return;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // Most block tags create this eagerly, but HTML allows bare text after a
  // self-closing element such as <hr/>. Keep malformed/minimal XHTML from
  // dereferencing a missing current block during recovery.
  if (!self->currentTextBlock) {
    LOG_ERR("EHP", "Missing text block before character data; starting a fallback body block");
    self->startNewTextBlock(BlockStyle{});
  }

  // Collect ruby text instead of normal word processing
  if (self->collectingRubyText) {
    self->rubyTextBuffer.append(s, len);
    return;
  }

  // Collect footnote link display text (for the number label)
  // Skip whitespace and brackets to normalize noterefs like "[1]" → "1"
  if (self->insideFootnoteLink) {
    for (int i = 0; i < len; i++) {
      unsigned char c = static_cast<unsigned char>(s[i]);
      if (isWhitespace(c) || c == '[' || c == ']') continue;
      if (self->currentFootnoteLinkTextLen < static_cast<int>(sizeof(self->currentFootnoteLinkText)) - 1) {
        self->currentFootnoteLinkText[self->currentFootnoteLinkTextLen++] = c;
        self->currentFootnoteLinkText[self->currentFootnoteLinkTextLen] = '\0';
      }
    }
  }

  int i = 0;
  while (i < len) {
    // Check for whitespace (ASCII only)
    if (isWhitespace(s[i])) {
      // Flush any buffered content as a word
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      // Horizontal layout supplies inter-word spacing itself. Vertical layout
      // stores every advance explicitly, so keep one collapsed separator and
      // emit it only if another word follows.
      if (self->verticalMode && self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
        self->pendingVerticalWhitespace = true;
      }
      self->nextWordContinues = false;
      i++;
      continue;
    }

    // Detect U+00A0 (non-breaking space, UTF-8: 0xC2 0xA0) or
    //        U+202F (narrow no-break space, UTF-8: 0xE2 0x80 0xAF).
    //
    // Both are rendered as a visible space but must never allow a line break around them.
    // We split the no-break space into its own word token and link the surrounding words
    // with continuation flags so the layout engine treats them as an indivisible group.
    if (static_cast<uint8_t>(s[i]) == 0xC2 && i + 1 < len && static_cast<uint8_t>(s[i + 1]) == 0xA0) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      self->flushPendingVerticalWhitespace();

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->nextWordContinues = true;  // Attach space to previous word (no break).
      self->flushPartWordBuffer();

      self->nextWordContinues = true;  // Next real word attaches to this space (no break).

      i++;  // Skip the second byte (0xA0)
      continue;
    }

    // U+202F (narrow no-break space) -- identical logic to U+00A0 above.
    if (static_cast<uint8_t>(s[i]) == 0xE2 && i + 2 < len && static_cast<uint8_t>(s[i + 1]) == 0x80 &&
        static_cast<uint8_t>(s[i + 2]) == 0xAF) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      self->flushPendingVerticalWhitespace();

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->nextWordContinues = true;
      self->flushPartWordBuffer();

      self->nextWordContinues = true;

      i += 2;  // Skip the remaining two bytes (0x80 0xAF)
      continue;
    }

    // Determine UTF-8 character length
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    const int charLen = getUtf8ByteLength(b0);
    if (i + charLen > len) {
      // Incomplete UTF-8 sequence at end, just add the byte
      if (self->partWordBufferIndex < MAX_WORD_SIZE) {
        self->partWordBuffer[self->partWordBufferIndex++] = s[i];
      }
      i++;
      continue;
    }

    // Decode the codepoint to check if it's CJK or invisible
    const uint32_t cp = decodeUtf8Codepoint(&s[i], charLen);

    // Skip invisible/zero-width Unicode characters that fonts can't render
    if (isInvisibleCodepoint(cp)) {
      i += charLen;
      continue;
    }

    if (isCjkCodepointForSplit(cp)) {
      // CJK character: flush any buffered content first
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      self->flushPendingVerticalWhitespace();

      self->ensureTextBlockCapacityForWord();

      // Add this CJK character as its own "word"
      char cjkWord[5] = {0};  // Max 4 bytes for UTF-8 + null terminator
      for (int j = 0; j < charLen && j < 4; j++) {
        cjkWord[j] = s[i + j];
      }
      // CJK is laid out as one character per word, but it remains inline
      // content. Preserve the active emphasis rather than resetting it to
      // Regular when splitting the input stream.
      const bool cjkBold = self->boldUntilDepth < self->depth || self->effectiveBold;
      const bool cjkItalic = self->italicUntilDepth < self->depth || self->effectiveItalic;
      const bool cjkUnderline = self->underlineUntilDepth < self->depth || self->effectiveUnderline;
      EpdFontFamily::Style cjkStyle = EpdFontFamily::REGULAR;
      if (cjkBold) cjkStyle = static_cast<EpdFontFamily::Style>(cjkStyle | EpdFontFamily::BOLD);
      if (cjkItalic) cjkStyle = static_cast<EpdFontFamily::Style>(cjkStyle | EpdFontFamily::ITALIC);
      if (cjkUnderline) cjkStyle = static_cast<EpdFontFamily::Style>(cjkStyle | EpdFontFamily::UNDERLINE);
      if (self->verticalMode) {
        self->currentTextBlock->addWord(cjkWord, cjkStyle, VerticalTextUtils::VerticalBehavior::Upright);
      } else {
        self->currentTextBlock->addWord(cjkWord, cjkStyle);
      }
      self->noteEmptyBlockContent();
      i += charLen;
      continue;
    }

    // Non-CJK character: buffer it
    self->flushPendingVerticalWhitespace();
    // If we're about to run out of space, flush the buffer first
    if (self->partWordBufferIndex + charLen >= MAX_WORD_SIZE) {
      self->flushPartWordBuffer();
    }

    // Add all bytes of this character to the buffer
    for (int j = 0; j < charLen; j++) {
      self->partWordBuffer[self->partWordBufferIndex++] = s[i + j];
    }
    i += charLen;
  }

  // Flush buffered words to free memory. The standard threshold is 750 words, but when free heap
  // is low we flush earlier to prevent abort() from vector reallocation failure (operator new
  // cannot return nullptr without std::nothrow, and C++ exceptions are disabled on ESP32).
  const size_t wordCount = self->currentTextBlock->size();
  const bool normalFlush = wordCount > 750;
  const bool earlyFlush = wordCount > 100 && ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_PARSING * 2;
  // A group ruby annotation is applied only when its closing </ruby> arrives.
  // Flushing its base words beforehand loses that span and can split or drop
  // the annotation, so defer the memory flush until the group is complete.
  if (!self->inRuby && (normalFlush || earlyFlush)) self->flushTextBlockForMemory();
}

void XMLCALL ChapterHtmlSlimParser::defaultHandlerExpand(void* userData, const XML_Char* s, const int len) {
  // Check if this looks like an entity reference (&...;)
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* utf8Value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (utf8Value != nullptr) {
      // Known entity: expand to its UTF-8 value
      characterData(userData, utf8Value, strlen(utf8Value));
      return;
    }
    // Unknown entity: preserve original &...; sequence
    characterData(userData, s, len);
    return;
  }
  // Not an entity we recognize - skip it
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool willPopStyleStack =
      !self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth - 1;
  const bool willClearBold = self->boldUntilDepth == self->depth - 1;
  const bool willClearItalic = self->italicUntilDepth == self->depth - 1;
  const bool willClearUnderline = self->underlineUntilDepth == self->depth - 1;

  const bool styleWillChange = willPopStyleStack || willClearBold || willClearItalic || willClearUnderline;
  const bool headerOrBlockTag = isHeaderOrBlock(name);
  const bool tableStructuralTag = isTableStructuralTag(name);

  if (self->tableDepth > 1 && strcmp(name, "table") == 0) {
    self->tableDepth -= 1;
    LOG_DBG("EHP", "nested table detected, skipped");
    return;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    if (!self->tableBuffer.empty()) {
      ChapterHtmlSlimParser::TableCellData cell;
      if (!self->tableCellTextBuffer.empty() && self->tableCellTextBuffer.back() == ' ') {
        self->tableCellTextBuffer.pop_back();
      }
      cell.text = std::move(self->tableCellTextBuffer);
      cell.isHeader = self->tableCellIsHeader;
      self->tableBuffer.back().cells.push_back(std::move(cell));
    }
    self->tableCellTextBuffer.clear();
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    // Row complete
  }

  if (self->tableDepth == 1 && strcmp(name, "table") == 0) {
    self->tableDepth -= 1;
    self->flushTableAsGrid();
    self->tableBuffer.clear();
    self->tableRowIndex = 0;
    self->tableColIndex = 0;

    // tableの後で改ページ — テーブルと後続テキストの重なりを防ぐ
    if (self->currentPage) {
      self->completeCurrentPage();
      self->currentPage.reset();
      self->currentPageNextY = 0;
      if (self->verticalMode) {
        const int lineHeight = self->renderer.getLineHeight(self->fontId) * self->lineCompression;
        self->currentPageNextX = self->viewportWidth - lineHeight;
      }
    }
  }

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag =
        !headerOrBlockTag && !tableStructuralTag && !matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS) && self->depth != 1;
    const bool shouldFlush = styleWillChange || headerOrBlockTag || matches(name, BOLD_TAGS, NUM_BOLD_TAGS) ||
                             matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS) ||
                             matches(name, UNDERLINE_TAGS, NUM_UNDERLINE_TAGS) || tableStructuralTag ||
                             matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS) || self->depth == 1;

    if (shouldFlush) {
      self->flushPartWordBuffer();
      // If closing an inline element, the next word fragment continues the same visual word
      if (isInlineTag) {
        self->nextWordContinues = true;
      }
    }
  }

  self->depth -= 1;

  const bool closedEmptyParagraph =
      (strcmp(name, "p") == 0 || strcmp(name, "div") == 0) && self->consumeEmptyBlockCandidate(self->depth);
  if (closedEmptyParagraph && self->currentTextBlock && self->currentTextBlock->isEmpty()) {
    // The explicit blank line is real content for any containing <div>, but
    // it must not reset the consecutive-blank-line limit.
    for (auto& candidate : self->emptyBlockCandidates) candidate.hasContent = true;
    self->addExplicitBlankLine();
    if (!self->currentTextBlock->isEmpty()) self->makePages();
  }

  // Ruby closing tags
  // The common close path above has already decremented depth once.  Ruby
  // elements must not decrement it a second time: doing so prevents an
  // enclosing inline element such as <b> from finding its style-stack entry.
  if (strcmp(name, "rt") == 0) {
    self->collectingRubyText = false;
    return;
  }

  if (strcmp(name, "rb") == 0) {
    self->flushPartWordBuffer();
    if (!self->inlineStyleStack.empty() && self->inlineStyleStack.back().rubyBaseTagStyle) {
      self->inlineStyleStack.pop_back();
      self->updateEffectiveInlineStyle();
    }
    return;
  }

  if (strcmp(name, "rp") == 0) {
    return;
  }

  if (strcmp(name, "ruby") == 0 && self->inRuby && self->currentTextBlock) {
    self->flushPartWordBuffer();

    const int currentWordCount = static_cast<int>(self->currentTextBlock->size());
    const int baseWordCount = currentWordCount - self->rubyStartWordIndex;

    if (baseWordCount > 0 && !self->rubyTextBuffer.empty()) {
      // Count UTF-8 characters in ruby text
      std::vector<size_t> charOffsets;
      const char* p = self->rubyTextBuffer.c_str();

      while (*p) {
        charOffsets.push_back(p - self->rubyTextBuffer.c_str());

        if ((*p & 0x80) == 0) {
          p += 1;
        } else if ((*p & 0xE0) == 0xC0) {
          p += 2;
        } else if ((*p & 0xF0) == 0xE0) {
          p += 3;
        } else {
          p += 4;
        }
      }

      charOffsets.push_back(self->rubyTextBuffer.size());
      const int rubyCharCount = static_cast<int>(charOffsets.size() - 1);

      // ルビを親文字ごとに分割せず、親文字の先頭にまとめて付ける。
      // 分割方式だと「スター」が「ス」「タ」のように欠けたり、
      // 親文字数とのズレで一部ルビが表示されないことがある。
      self->currentTextBlock->setRubyForWordAt(self->rubyStartWordIndex, self->rubyTextBuffer,
                                               static_cast<size_t>(baseWordCount));
    }

    self->collectingRubyText = false;
    self->inRuby = false;
    self->rubyStartWordIndex = -1;
    self->rubyTextBuffer.clear();

    if (!self->inlineStyleStack.empty() && self->inlineStyleStack.back().rubyTagStyle) {
      self->inlineStyleStack.pop_back();
      self->updateEffectiveInlineStyle();
    }
    return;
  }

  // Closing a footnote link — create entry from collected text and href
  if (self->insideFootnoteLink && self->depth == self->footnoteLinkDepth) {
    if (self->currentFootnoteLinkText[0] != '\0' && self->currentFootnoteLinkHref[0] != '\0') {
      FootnoteEntry entry;
      strncpy(entry.number, self->currentFootnoteLinkText, sizeof(entry.number) - 1);
      entry.number[sizeof(entry.number) - 1] = '\0';
      strncpy(entry.href, self->currentFootnoteLinkHref, sizeof(entry.href) - 1);
      entry.href[sizeof(entry.href) - 1] = '\0';
      int wordIndex =
          self->wordsExtractedInBlock + (self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0);
      self->pendingFootnotes.push_back({wordIndex, entry});
    }
    self->insideFootnoteLink = false;
  }

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  // Leaving bold tag
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic tag
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }

  // Leaving underline tag
  if (self->underlineUntilDepth == self->depth) {
    self->underlineUntilDepth = INT_MAX;
  }

  // Pop from inline style stack if we pushed an entry at this depth
  // This handles all inline elements: b, i, u, span, etc.
  if (!self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth) {
    self->inlineStyleStack.pop_back();
    self->updateEffectiveInlineStyle();
  }

  // Clear block style when leaving header or block elements
  if (headerOrBlockTag) {
    self->currentCssStyle.reset();
    self->updateEffectiveInlineStyle();

    // Reset block style on empty text blocks to prevent stale styling from bleeding
    // into the next sibling element. This fixes issue #1026 where an empty <h1> (default
    // Center) followed by an image-only <p> causes Center to persist through the chain
    // of empty block reuse into subsequent text paragraphs.
    // Margins/padding are also reset to prevent a closed block's horizontal margins from
    // leaking to the next sibling (e.g. <div style="margin:1em 40%"><img/></div><p>text</p>
    // would otherwise cause the <p> to inherit the div's 40% side margins).
    // Parent-child margin accumulation is unaffected because it happens at child open time
    // via getCombinedBlockStyle(), before the parent closes.
    if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
      auto style = self->currentTextBlock->getBlockStyle();
      style.textAlignDefined = false;
      style.alignment = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                            ? CssTextAlign::Justify
                            : static_cast<CssTextAlign>(self->paragraphAlignment);
      style.marginLeft = 0;
      style.marginRight = 0;
      style.marginTop = 0;
      style.marginBottom = 0;
      style.paddingLeft = 0;
      style.paddingRight = 0;
      style.paddingTop = 0;
      style.paddingBottom = 0;
      self->currentTextBlock->setBlockStyle(style);
    }
  }

  if (strcmp(name, "html") == 0) {
    self->htmlEnded = true;
  }
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  htmlEnded = false;
  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  // Resolve None sentinel to Justify for initial block (no CSS context yet)
  const auto align = (this->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                         ? CssTextAlign::Justify
                         : static_cast<CssTextAlign>(this->paragraphAlignment);
  paragraphAlignmentBlockStyle.alignment = align;
  startNewTextBlock(paragraphAlignmentBlockStyle);

  const XML_Parser parser = XML_ParserCreate(nullptr);
  int done;

  if (!parser) {
    LOG_ERR("EHP", "Couldn't allocate memory for parser");
    return false;
  }

  // Handle HTML entities (like &nbsp;) that aren't in XML spec or DTD
  // Using DefaultHandlerExpand preserves normal entity expansion from DOCTYPE
  XML_SetDefaultHandlerExpand(parser, defaultHandlerExpand);

  FsFile file;
  if (!Storage.openFileForRead("EHP", filepath, file)) {
    XML_ParserFree(parser);
    return false;
  }

  // Get file size to decide whether to show indexing popup.
  if (popupFn && file.size() >= MIN_SIZE_FOR_POPUP) {
    popupFn();
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  // Compute the time taken to parse and build pages
  const uint32_t chapterStartTime = millis();
  do {
    if (cancelFn && cancelFn()) {
      LOG_DBG("EHP", "Parsing cancelled by user");
      XML_StopParser(parser, XML_FALSE);
      XML_SetElementHandler(parser, nullptr, nullptr);
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    void* const buf = XML_GetBuffer(parser, PARSE_BUFFER_SIZE);
    if (!buf) {
      LOG_ERR("EHP", "Couldn't allocate memory for buffer");
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    const size_t len = file.read(buf, PARSE_BUFFER_SIZE);

    if (len == 0 && file.available() > 0) {
      LOG_ERR("EHP", "File read error");
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    done = file.available() == 0;

    if (XML_ParseBuffer(parser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      if (htmlEnded) {
        LOG_DBG("EHP", "Ignoring trailing data after </html>: %s", XML_ErrorString(XML_GetErrorCode(parser)));
        break;
      }
      LOG_ERR("EHP", "Parse error at line %lu:\n%s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    // Periodic heap check during parsing to prevent abort() from failed allocations
    if (!done && ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_PARSING) {
      LOG_ERR("EHP", "Low heap during parsing (%u bytes), stopping gracefully", ESP.getFreeHeap());
      XML_StopParser(parser, XML_FALSE);
      XML_SetElementHandler(parser, nullptr, nullptr);
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }
  } while (!done);
  LOG_DBG("EHP", "Time to parse and build pages: %lu ms", millis() - chapterStartTime);

  XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
  XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
  XML_SetCharacterDataHandler(parser, nullptr);
  XML_ParserFree(parser);
  file.close();

  // Process last page if there is still text
  if (currentTextBlock) {
    makePages();
    if (!pendingAnchorId.empty()) {
      anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
      pendingAnchorId.clear();
    }
    if (currentPage && !currentPage->elements.empty()) {
      completeCurrentPage();
    }
    currentPage.reset();
    currentTextBlock.reset();
  }

  return true;
}

void ChapterHtmlSlimParser::completeCurrentPage() {
  if (!currentPage) return;
  completePageFn(std::move(currentPage));
  completedPageCount++;
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line) {
  const int effectiveFontId = (line->getBlockStyle().fontId != 0) ? line->getBlockStyle().fontId : fontId;
  const int lineHeight =
      renderer.getLineHeight(effectiveFontId) * lineCompression * line->getBlockStyle().lineHeightMultiplier;

  if (verticalMode) {
    // Vertical mode: columns placed right-to-left
    const int columnWidth = lineHeight;
    // Preserve the common quarter-width gutter for ordinary columns.
    const int columnSpacing = columnWidth / 4;

    if (!currentPage) {
      currentPage.reset(new Page());
      currentPageNextX = viewportWidth - columnWidth;  // start from right edge
    }

    const auto rubyRightInset = [&]() {
      if (!line->hasRuby()) return 0;
      // Ruby is placed on the right of its base. Reserve only the part that
      // extends beyond the body column. Configured column spacing already
      // provides clearance between columns, so add only the missing amount;
      // the first column still has to clear the physical page edge entirely.
      const int overflow = TextBlock::getVerticalRubyRightOverflow(renderer, effectiveFontId, columnWidth);
      if (!currentPage || currentPage->elements.empty()) return overflow;
      return std::max(0, overflow - columnSpacing);
    };

    int columnX = currentPageNextX - rubyRightInset();
    if (columnX < 0) {
      // Page full — emit and start new page
      completeCurrentPage();
      currentPage.reset(new Page());
      currentPageNextX = viewportWidth - columnWidth;
      columnX = currentPageNextX - rubyRightInset();
    }

    // Track cumulative words for footnote assignment
    wordsExtractedInBlock += line->wordCount();
    auto footnoteIt = pendingFootnotes.begin();
    while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
      currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
      ++footnoteIt;
    }
    pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);

    // Column x-position, y=0 (column starts at top)
    currentPage->elements.push_back(std::make_shared<PageLine>(line, static_cast<int16_t>(columnX), 0));
    currentPageNextX = columnX - (columnWidth + columnSpacing);
  } else {
    // Horizontal mode: lines placed top-to-bottom (existing logic)
    if (!currentPage) {
      currentPage.reset(new Page());
      currentPageNextY = 0;
    }

    int rubyTopInset = 0;
    if (line->hasRuby()) {
      const int requiredBodyY = TextBlock::getHorizontalRubyTopInset(renderer, effectiveFontId);
      // Configured line spacing already contributes leading between body
      // lines. Add only the clearance still missing for ruby; the first line
      // has no preceding body line and only clears the page edge.
      if (currentPage->elements.empty()) {
        rubyTopInset = std::max(0, requiredBodyY - currentPageNextY);
      } else {
        const int bodyLineHeight = renderer.getLineHeight(effectiveFontId);
        const int existingLeading = std::max(0, lineHeight - bodyLineHeight);
        rubyTopInset = std::max(0, requiredBodyY - existingLeading);
      }
    }

    if (currentPageNextY + rubyTopInset + lineHeight > viewportHeight) {
      completeCurrentPage();
      currentPage.reset(new Page());
      currentPageNextY = 0;
      rubyTopInset = line->hasRuby() ? TextBlock::getHorizontalRubyTopInset(renderer, effectiveFontId) : 0;
    }

    // Track cumulative words for footnote assignment
    wordsExtractedInBlock += line->wordCount();
    auto footnoteIt = pendingFootnotes.begin();
    while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
      currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
      ++footnoteIt;
    }
    pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);

    const int16_t xOffset = line->getBlockStyle().leftInset();
    currentPage->elements.push_back(
        std::make_shared<PageLine>(line, xOffset, static_cast<int16_t>(currentPageNextY + rubyTopInset)));
    currentPageNextY += rubyTopInset + lineHeight;
  }
}

// Character-level text wrapping for table cells.
// Unlike wrappedText() which only breaks on spaces, this breaks at any character boundary,
// making it suitable for CJK text without spaces.
static std::vector<std::string> wrapCellText(GfxRenderer& renderer, const int fontId, const char* text,
                                             const int maxWidth, const int maxLines, const EpdFontFamily::Style style) {
  std::vector<std::string> lines;
  if (!text || !*text || maxWidth <= 0 || maxLines <= 0) return lines;

  const char* p = text;
  std::string current;

  while (*p) {
    // Determine UTF-8 character length
    int charLen = 1;
    const auto c = static_cast<unsigned char>(*p);
    if (c >= 0xF0)
      charLen = 4;
    else if (c >= 0xE0)
      charLen = 3;
    else if (c >= 0xC0)
      charLen = 2;

    std::string test = current + std::string(p, charLen);
    if (renderer.getTextWidth(fontId, test.c_str(), style) > maxWidth) {
      if (current.empty()) {
        // Single char wider than maxWidth — just push it
        lines.push_back(std::string(p, charLen));
        p += charLen;
      } else {
        lines.push_back(current);
        current.clear();
        // Don't advance p — re-process this character on the next line
      }
      if (static_cast<int>(lines.size()) >= maxLines) {
        // Append remaining text as truncated last line
        if (*p) {
          lines.back() = renderer.truncatedText(fontId, (lines.back() + std::string(p)).c_str(), maxWidth, style);
        }
        return lines;
      }
      continue;
    }
    current = test;
    p += charLen;
  }

  if (!current.empty() && static_cast<int>(lines.size()) < maxLines) {
    lines.push_back(current);
  }

  return lines;
}

void ChapterHtmlSlimParser::flushTableAsGrid() {
  if (tableBuffer.empty()) return;

  int maxCols = 0;
  for (const auto& row : tableBuffer) {
    maxCols = std::max(maxCols, static_cast<int>(row.cells.size()));
  }
  if (maxCols == 0) return;

  const int tblFontId = (tableFontId != 0) ? tableFontId : fontId;
  // Use compact line height for tables: ascender + 1px instead of full advanceY.
  const int ascender = renderer.getFontAscenderSize(tblFontId);
  const int fullLineH = renderer.getLineHeight(tblFontId);
  const int lineH = ascender + 1;
  // Descender overshoot: glyphs extend below lineH by this amount
  const int descenderExtra = std::max(0, fullLineH - lineH);
  static constexpr int16_t CELL_PADDING = 2;
  static constexpr int MAX_CELL_LINES = 8;

  // Limit columns to what can fit (min ~50px per column)
  static constexpr int MIN_COL_WIDTH = 50;
  const int maxFittableCols = std::max(1, static_cast<int>(viewportWidth) / MIN_COL_WIDTH);
  if (maxCols > maxFittableCols) maxCols = maxFittableCols;

  // Measure max content width per column across all rows
  std::vector<int> maxContentWidth(maxCols, 0);
  for (const auto& row : tableBuffer) {
    for (int col = 0; col < maxCols && col < static_cast<int>(row.cells.size()); col++) {
      if (!row.cells[col].text.empty()) {
        const auto style = row.cells[col].isHeader ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
        const int tw = renderer.getTextWidth(tblFontId, row.cells[col].text.c_str(), style);
        maxContentWidth[col] = std::max(maxContentWidth[col], tw);
      }
    }
  }

  // Column widths: content-based with minimum width, capped to viewport
  auto layout = std::make_shared<TableColumnLayout>();
  layout->colWidths.resize(maxCols);
  layout->fontId = tblFontId;
  layout->lineHeight = static_cast<int16_t>(lineH);
  layout->cellPadding = CELL_PADDING;

  static constexpr int MIN_COL_CONTENT_WIDTH = 30;
  int totalNeeded = 0;
  for (int col = 0; col < maxCols; col++) {
    int needed = std::max(MIN_COL_CONTENT_WIDTH, maxContentWidth[col]) + CELL_PADDING * 2;
    layout->colWidths[col] = static_cast<uint16_t>(needed);
    totalNeeded += needed;
  }

  if (totalNeeded > viewportWidth) {
    // Scale down proportionally to fit viewport
    for (int col = 0; col < maxCols; col++) {
      layout->colWidths[col] =
          static_cast<uint16_t>(static_cast<int>(layout->colWidths[col]) * viewportWidth / totalNeeded);
    }
  }

  // Start table on a new page if current page has content
  if (currentPage && currentPageNextY > 0) {
    completeCurrentPage();
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  // Process each row: wrap text, compute row height, add to pages
  const int numRows = static_cast<int>(tableBuffer.size());
  for (int rowIdx = 0; rowIdx < numRows; rowIdx++) {
    const auto& row = tableBuffer[rowIdx];
    std::vector<std::vector<std::string>> cellLines(maxCols);
    std::vector<bool> headers(maxCols, false);
    int maxLinesInRow = 1;

    for (int col = 0; col < maxCols && col < static_cast<int>(row.cells.size()); col++) {
      headers[col] = row.cells[col].isHeader;
      const auto style = headers[col] ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      const int maxTextW = layout->colWidths[col] - CELL_PADDING * 2;
      if (maxTextW > 0 && !row.cells[col].text.empty()) {
        cellLines[col] =
            wrapCellText(renderer, tblFontId, row.cells[col].text.c_str(), maxTextW, MAX_CELL_LINES, style);
      }
      maxLinesInRow = std::max(maxLinesInRow, static_cast<int>(cellLines[col].size()));
    }

    // Add descenderExtra so bottom padding visually matches top padding
    const int16_t rowHeight = static_cast<int16_t>(maxLinesInRow * lineH + descenderExtra + CELL_PADDING * 2);

    auto block = std::make_shared<TableRowBlock>(std::move(cellLines), std::move(headers), layout, rowHeight,
                                                 rowIdx == 0, rowIdx == numRows - 1);

    if (!currentPage) {
      currentPage.reset(new Page());
      currentPageNextY = 0;
    }

    if (currentPageNextY + rowHeight > viewportHeight) {
      completeCurrentPage();
      currentPage.reset(new Page());
      currentPageNextY = 0;
    }

    currentPage->elements.push_back(std::make_shared<PageTableRow>(block, 0, currentPageNextY));
    currentPageNextY += rowHeight;
  }
}

void ChapterHtmlSlimParser::makePages() {
  if (!currentTextBlock) {
    LOG_ERR("EHP", "!! No text block to make pages for !!");
    return;
  }

  const int lineHeight =
      renderer.getLineHeight(fontId) * lineCompression * currentTextBlock->getBlockStyle().lineHeightMultiplier;
  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();

  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
  const int layoutFontId = (blockStyle.fontId != 0) ? blockStyle.fontId : fontId;

  const auto discardExplicitBlankLine = [&]() {
    if (verticalMode) {
      currentTextBlock->layoutVerticalColumns(renderer, layoutFontId, viewportHeight,
                                              [](const std::shared_ptr<TextBlock>&) {});
    } else {
      currentTextBlock->layoutAndExtractLines(renderer, layoutFontId, effectiveWidth,
                                              [](const std::shared_ptr<TextBlock>&) {});
    }
  };

  // Keep a blank line at the tail of a page, but never create a new page or
  // column headed only by that blank line. Its margins are discarded as well.
  if (currentTextBlock->isExplicitBlankLine()) {
    const bool pageHasContent = currentPage && !currentPage->elements.empty();
    const bool wouldStartNewPage =
        verticalMode ? !pageHasContent || currentPageNextX < 0
                     : !pageHasContent || currentPageNextY + std::max(0, static_cast<int>(blockStyle.marginTop)) +
                                                  std::max(0, static_cast<int>(blockStyle.paddingTop)) + lineHeight >
                                              viewportHeight;
    if (wouldStartNewPage) {
      discardExplicitBlankLine();
      return;
    }
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
    if (verticalMode) currentPageNextX = viewportWidth - lineHeight;
  }

  // Apply top spacing before the paragraph (stored in pixels)
  if (blockStyle.marginTop > 0) {
    currentPageNextY += blockStyle.marginTop;
  }
  if (blockStyle.paddingTop > 0) {
    currentPageNextY += blockStyle.paddingTop;
  }

  if (verticalMode) {
    currentTextBlock->layoutVerticalColumns(
        renderer, layoutFontId, viewportHeight,
        [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); });
  } else {
    currentTextBlock->layoutAndExtractLines(
        renderer, layoutFontId, effectiveWidth,
        [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); });
  }

  // Fallback: transfer any remaining pending footnotes to current page.
  // Normally addLineToPage handles this via word-index tracking, but this catches
  // edge cases where a footnote's word index equals the exact block size.
  if (!pendingFootnotes.empty() && currentPage) {
    for (const auto& [idx, fn] : pendingFootnotes) {
      currentPage->addFootnote(fn.number, fn.href);
    }
    pendingFootnotes.clear();
  }

  // In vertical mode, h1/h2 advance through columns right-to-left. Apply their
  // existing after-block spacing to that axis so the following body column does
  // not sit immediately beside a chapter heading.
  const int blockBottomSpacing =
      std::max(0, static_cast<int>(blockStyle.marginBottom)) + std::max(0, static_cast<int>(blockStyle.paddingBottom));
  if (verticalMode && blockStyle.drawSeparatorBelow && blockBottomSpacing > 0) {
    currentPageNextX -= blockBottomSpacing;
  }

  // Apply bottom spacing after the paragraph (stored in pixels)
  if (blockStyle.marginBottom > 0) {
    currentPageNextY += blockStyle.marginBottom;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY += blockStyle.paddingBottom;
  }

  // Extra paragraph spacing uses the reader's five-level preset.  List items
  // get half the normal gap to avoid excessive spacing in tables of contents.
  if (extraParagraphSpacing != 0) {
    const int gap = (lineHeight * extraParagraphSpacing) / 6;
    currentPageNextY += blockStyle.isListItem ? (gap / 2) : gap;
  }
}
