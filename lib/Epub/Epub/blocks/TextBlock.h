#pragma once
#include <EpdFontFamily.h>
#include <HalStorage.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "Block.h"
#include "BlockStyle.h"

// Represents a line of text on a page
class TextBlock final : public Block {
 public:
  // インライン画像（本文中の文字として扱う画像）。sparse方式: words 内の画像マーカー(U+FFFC)の
  // Word の数だけを、マーカー出現順に保持する。画像でないWordの空要素は持たない（メモリ最小化）。
  struct InlineImage {
    std::string imagePath;
    int16_t width = 0;
    int16_t height = 0;
  };

 private:
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  BlockStyle blockStyle;
  std::vector<int16_t> wordYpos;  // vertical layout: y position within column
  bool isVertical = false;        // true when this block was laid out vertically
  std::vector<std::string> rubyTexts;
  std::vector<InlineImage> inlineImages;  // sparse: words 内の画像マーカーの数だけ（出現順）

 public:
  explicit TextBlock(std::vector<std::string> words, std::vector<int16_t> word_xpos,
                     std::vector<EpdFontFamily::Style> word_styles, const BlockStyle& blockStyle = BlockStyle(),
                     std::vector<int16_t> word_ypos = {}, bool vertical = false,
                     std::vector<std::string> ruby_texts = {}, std::vector<InlineImage> inline_images = {})
      : words(std::move(words)),
        wordXpos(std::move(word_xpos)),
        wordStyles(std::move(word_styles)),
        blockStyle(blockStyle),
        wordYpos(std::move(word_ypos)),
        isVertical(vertical),
        rubyTexts(std::move(ruby_texts)),
        inlineImages(std::move(inline_images)) {
    if (rubyTexts.size() < this->words.size()) {
      rubyTexts.resize(this->words.size());
    }
    // inlineImages は sparse方式（画像の数だけ）なので、words と同数に resize しない。
    // render では words[i] がマーカーかどうかで、inlineImages の該当要素を出現順に参照する。
    if (this->isVertical && wordYpos.size() < this->words.size()) {
      wordYpos.resize(this->words.size(), 0);
    }
  }
  ~TextBlock() override = default;
  const std::vector<InlineImage>& getInlineImages() const { return inlineImages; }
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  const BlockStyle& getBlockStyle() const { return blockStyle; }
  const std::vector<std::string>& getWords() const { return words; }
  const std::vector<int16_t>& getWordXpos() const { return wordXpos; }
  const std::vector<int16_t>& getWordYpos() const { return wordYpos; }
  bool getIsVertical() const { return isVertical; }
  bool hasRuby() const;
  void appendRubyText(std::string& out) const;
  static int getVerticalRubyRightOverflow(const GfxRenderer& renderer, int bodyFontId, int layoutColumnWidth);
  static int getHorizontalRubyTopInset(const GfxRenderer& renderer, int bodyFontId);
  const std::vector<std::string>& getRubyTexts() const { return rubyTexts; }
  // Marks additional base-text tokens that belong to the ruby group stored on the preceding token.
  static constexpr char RUBY_CONTINUATION_MARKER = '\x01';
  static bool isRubyContinuation(const std::string& ruby) {
    return ruby.size() == 1 && ruby[0] == RUBY_CONTINUATION_MARKER;
  }
  static int rubyFontId;  // アプリ層から設定されるルビフォントID（0=ルビ描画しない）
  bool isEmpty() override { return words.empty(); }
  size_t wordCount() const { return words.size(); }
  // given a renderer works out where to break the words into lines
  // パラメータは非const参照（インライン画像を ImageBlock::render で描画するため）。
  // メソッド自体はconstのまま。
  void render(GfxRenderer& renderer, int fontId, int x, int y, int viewportWidth = 0, int viewportHeight = 0,
              int viewportLeft = 0, int viewportTop = 0, int rubyOffsetX = 0, int rubyOffsetY = 0) const;
  void collectCodepoints(std::vector<uint32_t>& out, size_t max) const;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(FsFile& file) const;
  static std::unique_ptr<TextBlock> deserialize(FsFile& file);
};
