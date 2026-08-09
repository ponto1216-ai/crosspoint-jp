#pragma once

#include <EpdFontFamily.h>
#include <VerticalTextUtils.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  std::vector<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<bool> wordContinues;     // true = word attaches to previous (no space before it)
  std::vector<std::string> rubyTexts;  // words と並列、ルビなしは空文字列
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
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine, const GfxRenderer& renderer,
                   int fontId);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const bool hyphenationEnabled = false, const BlockStyle& blockStyle = BlockStyle(),
                      const bool firstLineIndent = false)
      : blockStyle(blockStyle), firstLineIndent(firstLineIndent), hyphenationEnabled(hyphenationEnabled) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false);
  void addWord(std::string word, EpdFontFamily::Style fontStyle, VerticalTextUtils::VerticalBehavior vBehavior,
               bool underline = false, bool attachToPrevious = false);
  // 本文中の文字として扱うインライン画像を追加する。words にダミー文字 U+FFFC を1Wordとして積み、
  // 画像情報（パス・寸法）は sparse な inlineImages に追加する（words 内のマーカー出現順に対応）。
  void addImage(std::string imagePath, int16_t width, int16_t height);
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  void setRubyForWordAt(size_t index, const std::string& ruby, size_t baseWordCount = 1);
  bool isEmpty() const { return words.empty(); }
  bool isExplicitBlankLine() const {
    return !blockStyle.isHtmlRule && words.size() == 1 && words.front() == "\xE2\x80\x8B";
  }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true);
  void layoutVerticalColumns(const GfxRenderer& renderer, int fontId, uint16_t columnHeight,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processColumn,
                             bool includeLastColumn = true);
};
