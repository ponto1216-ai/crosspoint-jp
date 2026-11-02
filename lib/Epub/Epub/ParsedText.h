#pragma once

#include <EpdFontFamily.h>
#include <VerticalTextUtils.h>

#include <deque>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  friend struct ParsedTextTestAccess;
  // A large <ruby> base must remain in memory until its <rt> is parsed.  A
  // vector<string> then needs one growing contiguous array of string objects;
  // on an X3's fragmented heap, that allocation can throw bad_alloc and abort
  // the firmware even when total free heap appears sufficient.  deque grows in
  // small blocks, so the largest allocation stays bounded.
  std::deque<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  // Allocated only when an inline CSS size differs from the block font.
  std::vector<int32_t> wordFontIds;
  std::vector<TextEmphasis> emphasis;  // empty until the first marked token
  std::vector<bool> wordContinues;     // true = word attaches to previous (no space before it)
  // True only when the source contained ordinary whitespace before this word.
  // CJK tokens normally have no inter-word gap, so this must remain separate
  // from wordContinues in order to preserve an author-written Japanese space.
  std::vector<bool> wordSpaceBefore;
  std::deque<std::string> rubyTexts;  // words と並列、ルビなしは空文字列
  std::vector<VerticalTextUtils::VerticalBehavior> wordVerticalBehaviors;
  // インライン画像（本文中の文字として扱う画像）。sparse方式: 画像のあるWordの情報だけを、
  // words 内の画像マーカー(U+FFFC)の出現順に保持する。画像でないWordの空要素は持たない
  // （メモリ最小化）。words の何番目のマーカーかで対応付ける。
  struct InlineImage {
    std::string imagePath;
    int16_t width = 0;
    int16_t height = 0;
  };
  std::vector<InlineImage> inlineImages;  // words 内の U+FFFC マーカーの出現順（画像数だけ）
  BlockStyle blockStyle;
  bool firstLineIndent;
  bool hyphenationEnabled;
  uint8_t tateChuYokoMaxDigits;
  bool firstLineConsumed = false;
  bool fallbackIndentApplied = false;
  bool layoutFailed_ = false;
  bool hasHeapForToken();
  void prewarmInlineFonts(const GfxRenderer& renderer, int blockFontId);
  bool admitLayout(size_t bytes, const char* stage);
  void consumePrefix(size_t count);
  std::unique_ptr<TextBlock> prepareBlock(size_t start, size_t end, bool vertical);

  void applyParagraphIndent();
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth, int spaceWidth,
                                        std::vector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                        std::vector<bool>& wordIsCjkVec);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  int spaceWidth, std::vector<uint16_t>& wordWidths,
                                                  std::vector<bool>& continuesVec, std::vector<bool>& wordIsCjkVec);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks,
                            std::vector<bool>* continuesVec = nullptr, std::vector<bool>* wordIsCjkVec = nullptr);
  void extractLine(size_t breakIndex, int pageWidth, int spaceWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& wordIsCjkVec,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<bool(std::unique_ptr<TextBlock>)>& processLine, const GfxRenderer& renderer,
                   int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const bool hyphenationEnabled = false, const BlockStyle& blockStyle = BlockStyle(),
                      const bool firstLineIndent = false, const uint8_t tateChuYokoMaxDigits = 2)
      : blockStyle(blockStyle),
        firstLineIndent(firstLineIndent),
        hyphenationEnabled(hyphenationEnabled),
        tateChuYokoMaxDigits(tateChuYokoMaxDigits >= 3 ? 3 : 2) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               bool spaceBefore = false, int fontId = 0);
  void addWord(std::string word, EpdFontFamily::Style fontStyle, VerticalTextUtils::VerticalBehavior vBehavior,
               bool underline = false, bool attachToPrevious = false, bool spaceBefore = false, int fontId = 0);
  // Adds one Unicode super/subscript digit to a preceding short ASCII token.
  // Refuses any operation that would allocate, so low-memory parsing simply
  // falls back to the ordinary separate-word path.
  bool appendVerticalFormulaDigit(char digit, bool superscript);
  bool appendVerticalFormulaText(const char* text);
  // 本文中の文字として扱うインライン画像を追加する。words にダミー文字 U+FFFC を1Wordとして積み、
  // 画像情報（パス・寸法）は sparse な inlineImages に追加する（words 内のマーカー出現順に対応）。
  void addImage(std::string imagePath, int16_t width, int16_t height);
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  void setEmphasisFrom(size_t start, TextEmphasis value);
  void setRubyForWordAt(size_t index, const std::string& ruby, size_t baseWordCount = 1);
  bool isEmpty() const { return words.empty(); }
  int maxFontLineHeight(const GfxRenderer& renderer, int blockFontId) const;
  bool hasConsumedFirstLine() const { return firstLineConsumed; }
  bool layoutFailed() const { return layoutFailed_; }
  bool isExplicitBlankLine() const {
    return !blockStyle.isHtmlRule && words.size() == 1 && words.front() == "\xE2\x80\x8B";
  }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<bool(std::unique_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true);
  void layoutVerticalColumns(const GfxRenderer& renderer, int fontId, uint16_t columnHeight,
                             const std::function<bool(std::unique_ptr<TextBlock>)>& processColumn,
                             bool includeLastColumn = true);
};
