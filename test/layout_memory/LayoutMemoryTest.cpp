#include <Epub/LayoutMemory.h>
#include <Epub/ParsedText.h>
#include <GfxRenderer.h>

#include <cassert>
#include <iostream>
#include <tuple>

static bool failTextBlock = false;
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  if (failTextBlock && size == sizeof(TextBlock)) return nullptr;
  return ::operator new(size);
}

int TextBlock::rubyFontId = 2;
int TextBlock::smallFontId = 0;
struct ParsedTextTestAccess {
  static auto snapshot(const ParsedText& p) {
    std::vector<std::tuple<std::string, int16_t, int16_t>> images;
    for (const auto& image : p.inlineImages) images.emplace_back(image.imagePath, image.width, image.height);
    return std::make_tuple(p.words, p.rubyTexts, p.wordStyles, p.wordContinues, p.wordSpaceBefore,
                           p.wordVerticalBehaviors, p.emphasis, images);
  }
};

ParsedText sample() {
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.textIndentDefined = true;
  ParsedText p(false, style);
  for (int i = 0; i < 12; ++i) p.addWord("字", EpdFontFamily::REGULAR);
  p.setRubyForWordAt(1, "じ", 2);
  p.setEmphasisFrom(4, TextEmphasis::FilledDot);
  p.addImage("/images/first-long-path.png", 20, 20);
  p.addWord("終", EpdFontFamily::BOLD);
  p.addImage("/images/second-long-path.png", 20, 20);
  return p;
}

template <class Callback>
void layout(ParsedText& p, const GfxRenderer& renderer, bool vertical, Callback callback, bool last = true) {
  if (vertical)
    p.layoutVerticalColumns(renderer, 1, 100, callback, last);
  else
    p.layoutAndExtractLines(renderer, 1, 100, callback, last);
}

uint32_t firstCodepointForTest(const std::string& word) {
  const auto* p = reinterpret_cast<const unsigned char*>(word.c_str());
  return utf8NextCodepoint(&p);
}

uint32_t lastCodepointForTest(const std::string& word) {
  size_t i = word.size() - 1;
  while (i > 0 && (static_cast<unsigned char>(word[i]) & 0xc0) == 0x80) --i;
  const auto* p = reinterpret_cast<const unsigned char*>(word.c_str() + i);
  return utf8NextCodepoint(&p);
}

void assertKinsokuBoundary(const std::vector<std::vector<std::string>>& blocks) {
  for (size_t i = 1; i < blocks.size(); ++i) {
    assert(!blocks[i - 1].empty() && !blocks[i].empty());
    const auto tail = lastCodepointForTest(blocks[i - 1].back());
    const auto head = firstCodepointForTest(blocks[i].front());
    assert(!VerticalTextUtils::isKinsokuTail(tail));
    assert(!VerticalTextUtils::isKinsokuHead(head));
    assert(!VerticalTextUtils::isKinsokuInseparablePair(tail, head));
  }
}

int main() {
  GfxRenderer renderer;
  {
    GfxRenderer tallRenderer;
    tallRenderer.tallLineHeight = true;
    const auto firstPosition = [&](const char* text) {
      BlockStyle style;
      style.alignment = CssTextAlign::Left;
      ParsedText paragraph(false, style, true);
      paragraph.addWord(text, EpdFontFamily::REGULAR);
      std::vector<int16_t> positions;
      paragraph.layoutVerticalColumns(tallRenderer, 1, 400, [&](const auto& block) {
        positions = block->getWordYpos();
        return true;
      });
      assert(!paragraph.layoutFailed() && positions.size() == 1);
      return positions.front();
    };
    assert(firstPosition("日") == 29);
    assert(firstPosition("Ａ") == 29);
    assert(firstPosition("ａ") == 29);
    assert(firstPosition("：") == 29);
    assert(firstPosition("；") == 29);
    assert(firstPosition("5") == 29);
    assert(firstPosition("α") == 29);
    assert(firstPosition("ABC") == 42);  // Existing halfwidth-run layout is unchanged.
  }
  {
    GfxRenderer tallRenderer;
    tallRenderer.tallLineHeight = true;
    ParsedText punctuation(false, BlockStyle(), false);
    using Behavior = VerticalTextUtils::VerticalBehavior;
    punctuation.addWord("…", EpdFontFamily::REGULAR, Behavior::Upright);
    punctuation.addWord("5", EpdFontFamily::REGULAR, Behavior::TateChuYoko);
    punctuation.addWord("‥", EpdFontFamily::REGULAR, Behavior::Upright);
    punctuation.addWord("6", EpdFontFamily::REGULAR, Behavior::TateChuYoko);
    std::vector<int16_t> positions;
    punctuation.layoutVerticalColumns(tallRenderer, 1, 400, [&](const auto& block) {
      positions = block->getWordYpos();
      return true;
    });
    assert(!punctuation.layoutFailed());
    assert((positions == std::vector<int16_t>{0, 22, 32, 54}));
    GfxRenderer notoRenderer;
    notoRenderer.tallLineHeight = true;
    notoRenderer.fontPath = "NotoSerifJp_14.cpfont";
    ParsedText noto(false, BlockStyle(), false);
    noto.addWord("…", EpdFontFamily::REGULAR, Behavior::Upright);
    noto.addWord("5", EpdFontFamily::REGULAR, Behavior::TateChuYoko);
    noto.addWord("‥", EpdFontFamily::REGULAR, Behavior::Upright);
    noto.addWord("6", EpdFontFamily::REGULAR, Behavior::TateChuYoko);
    noto.layoutVerticalColumns(notoRenderer, 1, 400, [&](const auto& block) {
      positions = block->getWordYpos();
      return true;
    });
    assert(!noto.layoutFailed());
    assert((positions == std::vector<int16_t>{0, 29, 39, 68}));
  }
  // The first-column indent is a logical cell offset. Bracket ink and
  // decoration strokes can extend above the body glyph while keeping it.
  for (const auto& [firstWord, style] :
       {std::pair{"「", EpdFontFamily::REGULAR}, std::pair{"ア", EpdFontFamily::UNDERLINE},
        std::pair{"打", EpdFontFamily::STRIKETHROUGH}, std::pair{"ﾏ", EpdFontFamily::REGULAR}}) {
    ESP = {};
    BlockStyle blockStyle;
    blockStyle.alignment = CssTextAlign::Left;
    ParsedText paragraph(false, blockStyle, true);
    paragraph.addWord(firstWord, style);
    paragraph.addWord("字", EpdFontFamily::REGULAR);
    std::vector<int16_t> positions;
    paragraph.layoutVerticalColumns(renderer, 1, 100, [&](const auto& block) {
      positions = block->getWordYpos();
      return true;
    });
    assert(!paragraph.layoutFailed() && paragraph.isEmpty() && !positions.empty() && positions.front() == 20);
  }
  assert(VerticalTextUtils::isVerticalGlyphCell(0xFF8F));  // ﾏ
  assert(VerticalTextUtils::isVerticalGlyphCell(0xFF9E));  // ﾞ
  {
    ESP = {};
    GfxRenderer narrowRenderer;
    narrowRenderer.narrowHalfwidth = true;
    BlockStyle style;
    style.alignment = CssTextAlign::Left;
    ParsedText halfwidth(false, style, true);
    for (const char* word : {"ﾏ", "ｼ", "ﾞ", "ｶ"}) halfwidth.addWord(word, EpdFontFamily::REGULAR);
    std::vector<int16_t> positions;
    halfwidth.layoutVerticalColumns(narrowRenderer, 1, 100, [&](const auto& block) {
      positions = block->getWordYpos();
      return true;
    });
    assert(!halfwidth.layoutFailed() && halfwidth.isEmpty());
    assert((positions == std::vector<int16_t>{20, 40, 60, 60}));
  }
  // A memory flush keeps the final line/column and later resumes the same paragraph.
  // Only the actual paragraph start may receive a first-line indent.
  for (const bool vertical : {false, true}) {
    for (const bool indent : {false, true}) {
      for (const bool hyphenation : {false, true}) {
        BlockStyle style;
        style.alignment = CssTextAlign::Left;
        ParsedText paragraph(hyphenation, style, indent);
        for (int i = 0; i < 30; ++i) paragraph.addWord("字", EpdFontFamily::REGULAR);
        std::vector<int> starts;
        std::vector<std::string> firstWords;
        const auto collect = [&](std::unique_ptr<TextBlock> block) {
          starts.push_back(vertical ? block->getWordYpos().front() : block->getWordXpos().front());
          firstWords.push_back(block->getWords().front());
          return true;
        };
        if (vertical)
          paragraph.layoutVerticalColumns(renderer, 1, 100, collect, false);
        else
          paragraph.layoutAndExtractLines(renderer, 1, 100, collect, false);
        const size_t continuation = starts.size();
        assert(continuation > 0 && !paragraph.isEmpty());
        for (int i = 0; i < 20; ++i) paragraph.addWord("字", EpdFontFamily::REGULAR);
        if (vertical)
          paragraph.layoutVerticalColumns(renderer, 1, 100, collect);
        else
          paragraph.layoutAndExtractLines(renderer, 1, 100, collect);
        assert(!paragraph.layoutFailed() && paragraph.isEmpty() && starts.size() > continuation);
        assert(starts[0] == (indent ? 20 : 0));
        assert(starts[continuation] == 0);
        if (!vertical && !indent) {
          assert(firstWords[0] ==
                 "\xe2\x80\x83"
                 "字");
          assert(firstWords[continuation] == "字");
        }
      }
    }
  }
  {
    // A partial pass with no complete line must leave the real first line indented.
    BlockStyle style;
    style.alignment = CssTextAlign::Left;
    ParsedText paragraph(false, style, true);
    for (int i = 0; i < 3; ++i) paragraph.addWord("字", EpdFontFamily::REGULAR);
    int emitted = 0;
    paragraph.layoutAndExtractLines(
        renderer, 1, 100,
        [&](std::unique_ptr<TextBlock>) {
          ++emitted;
          return true;
        },
        false);
    assert(emitted == 0);
    for (int i = 0; i < 3; ++i) paragraph.addWord("字", EpdFontFamily::REGULAR);
    paragraph.layoutAndExtractLines(renderer, 1, 100, [&](std::unique_ptr<TextBlock> line) {
      if (emitted++ == 0) assert(line->getWordXpos().front() == 20);
      return true;
    });
    assert(!paragraph.layoutFailed() && paragraph.isEmpty() && emitted > 0);
  }
  for (bool vertical : {false, true}) {
    ESP = {};
    auto p = sample();
    ESP.checks = 0;  // Count layout probes independently of source construction.
    std::vector<std::string> rendered;
    std::vector<std::string> imagePaths;
    layout(p, renderer, vertical, [&](const auto& block) {
      rendered.insert(rendered.end(), block->getWords().begin(), block->getWords().end());
      for (const auto& image : block->getInlineImages()) imagePaths.push_back(image.imagePath);
      assert(block->getRubyTexts().size() == block->getWords().size());
      return true;
    });
    assert(!p.layoutFailed() && p.isEmpty() && rendered.size() == 15);
    assert((imagePaths == std::vector<std::string>{"/images/first-long-path.png", "/images/second-long-path.png"}));
    const int totalChecks = ESP.checks;

    // The consumer owns accepted output beyond the lifetime of ParsedText.
    std::vector<std::unique_ptr<TextBlock>> retained;
    {
      auto ownedSource = sample();
      layout(ownedSource, renderer, vertical, [&](std::unique_ptr<TextBlock> block) {
        retained.push_back(std::move(block));
        return true;
      });
      assert(ownedSource.isEmpty());
    }
    std::vector<std::string> ownedWords;
    for (const auto& block : retained) {
      ownedWords.insert(ownedWords.end(), block->getWords().begin(), block->getWords().end());
    }
    assert(ownedWords == rendered);
    retained.clear();

    // Fail every admission point. The remaining source must exactly equal
    // the suffix after the blocks accepted before the failure, metadata too.
    for (int failure = 0; failure < totalChecks; ++failure) {
      ESP = {};
      auto candidate = sample();
      size_t accepted = 0;
      ESP.checks = 0;
      ESP.failAt = failure;
      layout(candidate, renderer, vertical, [&](const auto& block) {
        accepted += block->wordCount();
        return true;
      });
      assert(candidate.layoutFailed());
      assert(candidate.size() == 15 - accepted);
      ESP = {};
      auto reference = sample();
      size_t referenceAccepted = 0;
      layout(reference, renderer, vertical, [&](const auto& block) {
        if (referenceAccepted == accepted) return false;
        referenceAccepted += block->wordCount();
        return true;
      });
      assert(ParsedTextTestAccess::snapshot(candidate) == ParsedTextTestAccess::snapshot(reference));
    }

    // Rejection by the page/cache callback also preserves the rejected block.
    for (int reject = 0; reject < 3; ++reject) {
      ESP = {};
      auto rejected = sample();
      size_t accepted = 0;
      int calls = 0;
      layout(rejected, renderer, vertical, [&](const auto& block) {
        if (calls++ == reject) return false;
        accepted += block->wordCount();
        return true;
      });
      assert(rejected.layoutFailed() && rejected.size() == 15 - accepted);
      const auto snapshot = ParsedTextTestAccess::snapshot(rejected);
      layout(rejected, renderer, vertical, [](const auto&) {
        assert(false);
        return true;
      });
      rejected.addWord("discarded after failure", EpdFontFamily::REGULAR);
      assert(snapshot == ParsedTextTestAccess::snapshot(rejected));
    }

    ESP = {};
    auto fragmented = sample();
    const auto before = ParsedTextTestAccess::snapshot(fragmented);
    ESP.largest = 128;
    layout(fragmented, renderer, vertical, [](const auto&) {
      assert(false);
      return true;
    });
    assert(fragmented.layoutFailed() && before == ParsedTextTestAccess::snapshot(fragmented));

    ESP = {};
    auto objectFailure = sample();
    failTextBlock = true;
    layout(objectFailure, renderer, vertical, [](const auto&) {
      assert(false);
      return true;
    });
    failTextBlock = false;
    assert(objectFailure.layoutFailed() && before == ParsedTextTestAccess::snapshot(objectFailure));

    ESP = {};
    renderer.sd = renderer.exhaustPrewarm = true;
    auto warm = sample();
    layout(warm, renderer, vertical, [](const auto&) {
      assert(false);
      return true;
    });
    assert(warm.layoutFailed() && warm.size() == 15);
    renderer.sd = renderer.exhaustPrewarm = false;

    ESP = {};
    auto partial = sample();
    size_t count = 0;
    layout(
        partial, renderer, vertical,
        [&](const auto& b) {
          count += b->wordCount();
          return true;
        },
        false);
    assert(!partial.layoutFailed() && !partial.isEmpty());
    layout(partial, renderer, vertical, [&](const auto& b) {
      count += b->wordCount();
      return true;
    });
    assert(partial.isEmpty() && count == 15);
  }
  // Real Hyphenator and production sideways splitting remain usable, including
  // the parallel metadata after a long Latin run is split into multiple words.
  for (bool vertical : {false, true}) {
    ESP = {};
    BlockStyle style;
    style.textIndentDefined = true;
    ParsedText longWord(true, style);
    longWord.addWord("abcdefghijklmnopqrstuv", EpdFontFamily::BOLD, VerticalTextUtils::VerticalBehavior::Sideways);
    longWord.setEmphasisFrom(0, TextEmphasis::FilledDot);
    std::string joined;
    layout(longWord, renderer, vertical, [&](const auto& b) {
      for (const auto& word : b->getWords())
        for (char c : word)
          if (c != '-') joined += c;
      return true;
    });
    assert(!longWord.layoutFailed() && longWord.isEmpty());
    assert(joined == "abcdefghijklmnopqrstuv");
  }
  ESP = {};
  BlockStyle softStyle;
  softStyle.textIndentDefined = true;
  ParsedText soft(false, softStyle);
  soft.addWord(
      "ab\xc2\xad"
      "cd",
      EpdFontFamily::REGULAR);
  const auto softBefore = ParsedTextTestAccess::snapshot(soft);
  layout(soft, renderer, false, [](const auto& b) {
    assert(b->getWords().front() == "abcd");
    return false;
  });
  assert(soft.layoutFailed() && softBefore == ParsedTextTestAccess::snapshot(soft));

  // CJK tokens normally have zero gap, but a literal source space must stay
  // visible.  The parser records this separately from inline continuations.
  ESP = {};
  BlockStyle spacingStyle;
  spacingStyle.textIndentDefined = true;
  ParsedText cjkSpace(false, spacingStyle);
  cjkSpace.addWord("日", EpdFontFamily::REGULAR);
  cjkSpace.addWord("本", EpdFontFamily::REGULAR);
  cjkSpace.addWord("語", EpdFontFamily::REGULAR, false, false, true);
  std::vector<int16_t> cjkXpos;
  cjkSpace.layoutAndExtractLines(renderer, 1, 100, [&](const auto& block) {
    cjkXpos = block->getWordXpos();
    return true;
  });
  assert(!cjkSpace.layoutFailed() && cjkSpace.isEmpty());
  assert((cjkXpos == std::vector<int16_t>{0, 20, 45}));

  // PR #144 の不足していた禁則分類を、既存の横/縦レイアウト経路で確認する。
  // 中点・開き括弧・連続する省略記号が境界に来る幅で組み、境界違反が残らないことを見る。
  assert(VerticalTextUtils::isKinsokuHead(0x30FB));  // ・
  assert(VerticalTextUtils::isKinsokuHead(0x309D));  // ゝ
  assert(VerticalTextUtils::isKinsokuTail(0x201C));  // “
  assert(VerticalTextUtils::isKinsokuTail(0x3012));  // 〒
  assert(VerticalTextUtils::isKinsokuInseparablePair(0x2026, 0x2026));
  assert(VerticalTextUtils::isKinsokuHead(0x31F0));  // ㇰ
  assert(VerticalTextUtils::isKinsokuHead(0x31FF));  // ㇿ
  {
    // The parser supplies one token per small katakana. Horizontal layout
    // must keep each off the line head without treating the run as a word.
    ESP = {};
    BlockStyle style;
    style.textIndentDefined = true;
    ParsedText ainu(false, style);
    for (const char* word :
         {"カ", "タ", "カ", "ナ", "：", "ア", "ㇰ", "イ", "ㇱ", "ウ", "ㇲ", "エ", "ㇳ", "オ", "ㇴ", "ハ", "ㇵ"})
      ainu.addWord(word, EpdFontFamily::REGULAR);
    std::vector<std::vector<std::string>> lines;
    ainu.layoutAndExtractLines(renderer, 1, 100, [&](const auto& block) {
      lines.push_back(block->getWords());
      return true;
    });
    assert(!ainu.layoutFailed() && ainu.isEmpty() && lines.size() > 1);
    assertKinsokuBoundary(lines);
  }
  for (bool vertical : {false, true}) {
    ESP = {};
    BlockStyle style;
    style.textIndentDefined = true;
    ParsedText punctuation(false, style);
    for (const char* word : {"あ", "い", "・", "う", "「", "え", "お"}) {
      punctuation.addWord(word, EpdFontFamily::REGULAR);
    }
    std::vector<std::vector<std::string>> blocks;
    if (vertical) {
      punctuation.layoutVerticalColumns(renderer, 1, 40, [&](const auto& block) {
        blocks.push_back(block->getWords());
        return true;
      });
    } else {
      punctuation.layoutAndExtractLines(renderer, 1, 40, [&](const auto& block) {
        blocks.push_back(block->getWords());
        return true;
      });
    }
    assert(!punctuation.layoutFailed() && punctuation.isEmpty());
    assertKinsokuBoundary(blocks);

    ESP = {};
    ParsedText ellipsis(false, style);
    ellipsis.addWord("あ", EpdFontFamily::REGULAR);
    // General Punctuation の省略記号も、和文本文では語間空白なしで前後の字に続く。
    // CJK 文字を個別トークンにする実機パーサと同じ境界を作る。
    for (const char* word : {"…", "…"}) ellipsis.addWord(word, EpdFontFamily::REGULAR, false, true);
    ellipsis.addWord("い", EpdFontFamily::REGULAR);
    blocks.clear();
    if (vertical) {
      ellipsis.layoutVerticalColumns(renderer, 1, 40, [&](const auto& block) {
        blocks.push_back(block->getWords());
        return true;
      });
    } else {
      ellipsis.layoutAndExtractLines(renderer, 1, 40, [&](const auto& block) {
        blocks.push_back(block->getWords());
        return true;
      });
    }
    assert(!ellipsis.layoutFailed() && ellipsis.isEmpty());
    assertKinsokuBoundary(blocks);
  }
  // Narrow horizontal ellipses still consume full vertical cells.
  {
    GfxRenderer narrowRenderer;
    narrowRenderer.narrowEllipsis = true;
    BlockStyle style;
    style.textIndentDefined = true;
    ParsedText ellipsis(false, style);
    for (const char* word : {"あ", "…", "…", "い", "う"}) ellipsis.addWord(word, EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> columns;
    ellipsis.layoutVerticalColumns(narrowRenderer, 1, 80, [&](std::unique_ptr<TextBlock> column) {
      columns.push_back(std::move(column));
      return true;
    });
    assert(!ellipsis.layoutFailed() && columns.size() == 2);
    assert(columns[0]->getWords().size() == 4 && columns[1]->getWords().front() == "う");
    for (size_t i = 0; i < columns[0]->getWords().size(); ++i)
      assert(columns[0]->getWordYpos()[i] == static_cast<int>(i * 20));
  }
  // Ellipses, full-height long marks, and a closing bracket must keep their
  // positions when a paragraph is laid out in separate memory-safe batches.
  {
    GfxRenderer narrowRenderer;
    narrowRenderer.narrowEllipsis = true;
    BlockStyle style;
    style.textIndentDefined = true;
    constexpr const char* text[] = {"あ", "い", "…",  "…",  "」", "う", "ー", "え", "―",
                                    "―",  "「", "お", "か", "‥",  "‥",  "き", "く"};
    auto collect = [](std::vector<std::vector<std::string>>& words, std::vector<std::vector<int16_t>>& positions) {
      return [&](std::unique_ptr<TextBlock> column) {
        const auto& ys = column->getWordYpos();
        for (size_t i = 1; i < ys.size(); ++i) assert(ys[i] > ys[i - 1]);
        assert(ys.back() + 20 <= 80);
        words.push_back(column->getWords());
        positions.push_back(ys);
        return true;
      };
    };
    ParsedText whole(false, style);
    for (const char* word : text) whole.addWord(word, EpdFontFamily::REGULAR);
    std::vector<std::vector<std::string>> wholeWords;
    std::vector<std::vector<int16_t>> wholePositions;
    whole.layoutVerticalColumns(narrowRenderer, 1, 80, collect(wholeWords, wholePositions));
    assert(!whole.layoutFailed() && whole.isEmpty());
    assertKinsokuBoundary(wholeWords);

    ParsedText batched(false, style);
    for (size_t i = 0; i < 9; ++i) batched.addWord(text[i], EpdFontFamily::REGULAR);
    std::vector<std::vector<std::string>> batchedWords;
    std::vector<std::vector<int16_t>> batchedPositions;
    batched.layoutVerticalColumns(narrowRenderer, 1, 80, collect(batchedWords, batchedPositions), false);
    assert(!batched.layoutFailed() && !batched.isEmpty() && !batchedWords.empty());
    for (size_t i = 9; i < std::size(text); ++i) batched.addWord(text[i], EpdFontFamily::REGULAR);
    batched.layoutVerticalColumns(narrowRenderer, 1, 80, collect(batchedWords, batchedPositions));
    assert(!batched.layoutFailed() && batched.isEmpty());
    assert(batchedWords == wholeWords && batchedPositions == wholePositions);
  }
  // A sideways pair of horizontal bars needs a small gap before the next
  // upright glyph; a single bar keeps its existing advance.
  for (const char* bars : {"―", "――"}) {
    BlockStyle style;
    style.textIndentDefined = true;
    ParsedText dash(false, style);
    dash.addWord(bars, EpdFontFamily::REGULAR, VerticalTextUtils::VerticalBehavior::Sideways);
    dash.addWord("の", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> columns;
    dash.layoutVerticalColumns(renderer, 1, 120, [&](std::unique_ptr<TextBlock> column) {
      columns.push_back(std::move(column));
      return true;
    });
    assert(!dash.layoutFailed() && columns.size() == 1);
    assert(columns[0]->getWordYpos()[1] == (std::string(bars) == "――" ? 45 : 20));
  }
  // The added gap may send the following glyph to the next column, while
  // keeping the two-bar token intact and inside the current column.
  {
    BlockStyle style;
    style.textIndentDefined = true;
    ParsedText dash(false, style);
    dash.addWord("あ", EpdFontFamily::REGULAR);
    dash.addWord("――", EpdFontFamily::REGULAR, VerticalTextUtils::VerticalBehavior::Sideways);
    dash.addWord("の", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> columns;
    dash.layoutVerticalColumns(renderer, 1, 80, [&](std::unique_ptr<TextBlock> column) {
      columns.push_back(std::move(column));
      return true;
    });
    assert(!dash.layoutFailed() && columns.size() == 2);
    assert(columns[0]->getWords().size() == 2 && columns[0]->getWords()[1] == "――");
    assert(columns[0]->getWordYpos()[1] == 20);
    assert(columns[1]->getWords().size() == 1 && columns[1]->getWords()[0] == "の");
  }
  // A long plain column can save one pixel per gap to retain a final 、 or 。
  // in its own cell. A short full column still uses the old kinsoku break.
  for (const char* mark : {"、", "。"}) {
    ESP = {};
    BlockStyle style;
    style.textIndentDefined = true;
    ParsedText punctuation(false, style);
    for (int i = 0; i < 20; ++i) punctuation.addWord("あ", EpdFontFamily::REGULAR);
    punctuation.addWord(mark, EpdFontFamily::REGULAR);
    punctuation.addWord("え", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> columns;
    punctuation.layoutVerticalColumns(renderer, 1, 400, [&](std::unique_ptr<TextBlock> column) {
      columns.push_back(std::move(column));
      return true;
    });
    assert(!punctuation.layoutFailed() && columns.size() == 2);
    assert(columns[0]->getWords().size() == 21 && columns[1]->getWords().front() == "え");
    assert(columns[0]->getWordYpos()[20] == 380);
    assert(columns[0]->getWordYpos()[20] > columns[0]->getWordYpos()[19]);

    // A completely full column has no room to separate the punctuation;
    // preserve the old kinsoku break rather than overlaying it on the body.
    ParsedText fullColumn(false, style);
    for (const char* word : {"あ", "い", "う"}) fullColumn.addWord(word, EpdFontFamily::REGULAR);
    fullColumn.addWord(mark, EpdFontFamily::REGULAR);
    fullColumn.addWord("え", EpdFontFamily::REGULAR);
    columns.clear();
    fullColumn.layoutVerticalColumns(renderer, 1, 60, [&](std::unique_ptr<TextBlock> column) {
      columns.push_back(std::move(column));
      return true;
    });
    assert(!fullColumn.layoutFailed() && columns.size() == 2);
    assert(columns[0]->getWords().size() == 2 && columns[1]->getWords()[1] == mark);
    assert(columns[1]->getWordYpos()[1] > columns[1]->getWordYpos()[0]);
  }
  // Small kana follow the same bounded gap adjustment only in plain columns.
  for (const char* mark : {"ぁ", "ぃ", "ぅ", "ぇ", "ぉ", "っ", "ゃ", "ゅ", "ょ"}) {
    ESP = {};
    BlockStyle style;
    style.textIndentDefined = true;
    ParsedText smallKana(false, style);
    for (int i = 0; i < 20; ++i) smallKana.addWord("あ", EpdFontFamily::REGULAR);
    smallKana.addWord(mark, EpdFontFamily::REGULAR);
    smallKana.addWord("え", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> columns;
    smallKana.layoutVerticalColumns(renderer, 1, 400, [&](std::unique_ptr<TextBlock> column) {
      columns.push_back(std::move(column));
      return true;
    });
    assert(!smallKana.layoutFailed() && columns.size() == 2);
    assert(columns[0]->getWords().size() == 21 && columns[0]->getWords().back() == mark);
    assert(columns[1]->getWords().front() == "え");
    assert(columns[0]->getWordYpos()[20] == 380);
    for (size_t i = 1; i < columns[0]->getWords().size(); ++i)
      assert(columns[0]->getWordYpos()[i] > columns[0]->getWordYpos()[i - 1]);

    ParsedText shortColumn(false, style);
    for (const char* word : {"あ", "い", "う"}) shortColumn.addWord(word, EpdFontFamily::REGULAR);
    shortColumn.addWord(mark, EpdFontFamily::REGULAR);
    shortColumn.addWord("え", EpdFontFamily::REGULAR);
    columns.clear();
    shortColumn.layoutVerticalColumns(renderer, 1, 60, [&](std::unique_ptr<TextBlock> column) {
      columns.push_back(std::move(column));
      return true;
    });
    assert(!shortColumn.layoutFailed() && columns.size() == 2);
    assert(columns[0]->getWords().size() == 2 && columns[1]->getWords()[1] == mark);
  }
  // A closing quote may use the same bounded gap adjustment while keeping
  // its own cell. If the deficit is too large, pull back instead of hanging.
  {
    ESP = {};
    BlockStyle style;
    style.textIndentDefined = true;
    ParsedText closingQuote(false, style);
    for (int i = 0; i < 20; ++i) closingQuote.addWord("あ", EpdFontFamily::REGULAR);
    closingQuote.addWord("」", EpdFontFamily::REGULAR);
    closingQuote.addWord("え", EpdFontFamily::REGULAR);
    std::vector<std::unique_ptr<TextBlock>> columns;
    closingQuote.layoutVerticalColumns(renderer, 1, 400, [&](std::unique_ptr<TextBlock> column) {
      columns.push_back(std::move(column));
      return true;
    });
    assert(!closingQuote.layoutFailed() && columns.size() == 2);
    assert(columns[0]->getWords().size() == 21 && columns[0]->getWords().back() == "」");
    assert(columns[0]->getWordYpos()[20] == 380 && columns[1]->getWords().front() == "え");

    // The punctuation pair is tightened without sharing a glyph cell.
    for (const char* bracket : {"」", "「"}) {
      ParsedText punctuationPair(false, style);
      punctuationPair.addWord("あ", EpdFontFamily::REGULAR);
      punctuationPair.addWord("。", EpdFontFamily::REGULAR);
      punctuationPair.addWord(bracket, EpdFontFamily::REGULAR);
      punctuationPair.addWord("い", EpdFontFamily::REGULAR);
      columns.clear();
      punctuationPair.layoutVerticalColumns(renderer, 1, 400, [&](std::unique_ptr<TextBlock> column) {
        columns.push_back(std::move(column));
        return true;
      });
      assert(!punctuationPair.layoutFailed() && columns.size() == 1);
      const auto& ys = columns[0]->getWordYpos();
      assert(ys.size() == 4 && ys[2] > ys[1] && ys[2] - ys[1] < ys[1] - ys[0]);
      const int bodyAdvance = renderer.getTextAdvanceX(1, "あ", EpdFontFamily::REGULAR);
      assert(ys[1] - ys[0] - (ys[2] - ys[1]) == std::max(1, bodyAdvance * 5 / 8));
      assert(ys[3] - ys[2] == ys[1] - ys[0]);
    }
    ParsedText styledPair(false, style);
    styledPair.addWord("あ", EpdFontFamily::REGULAR);
    styledPair.addWord("。", EpdFontFamily::REGULAR);
    styledPair.addWord("」", EpdFontFamily::BOLD);
    columns.clear();
    styledPair.layoutVerticalColumns(renderer, 1, 400, [&](std::unique_ptr<TextBlock> column) {
      columns.push_back(std::move(column));
      return true;
    });
    assert(!styledPair.layoutFailed() && columns.size() == 1);
    assert(columns[0]->getWordYpos()[2] - columns[0]->getWordYpos()[1] ==
           columns[0]->getWordYpos()[1] - columns[0]->getWordYpos()[0]);

    ParsedText quotedSentence(false, style);
    quotedSentence.addWord("「", EpdFontFamily::REGULAR);
    for (int i = 0; i < 19; ++i) quotedSentence.addWord("あ", EpdFontFamily::REGULAR);
    quotedSentence.addWord("」", EpdFontFamily::REGULAR);
    quotedSentence.addWord("え", EpdFontFamily::REGULAR);
    columns.clear();
    quotedSentence.layoutVerticalColumns(renderer, 1, 400, [&](std::unique_ptr<TextBlock> column) {
      columns.push_back(std::move(column));
      return true;
    });
    assert(!quotedSentence.layoutFailed() && columns.size() == 2);
    assert(columns[0]->getWords().front() == "「" && columns[0]->getWords().back() == "」");
    assert(columns[0]->getWordYpos()[20] == 380 && columns[1]->getWords().front() == "え");

    ParsedText shortColumn(false, style);
    for (const char* word : {"あ", "い", "う"}) shortColumn.addWord(word, EpdFontFamily::REGULAR);
    shortColumn.addWord("」", EpdFontFamily::REGULAR);
    shortColumn.addWord("え", EpdFontFamily::REGULAR);
    columns.clear();
    shortColumn.layoutVerticalColumns(renderer, 1, 60, [&](std::unique_ptr<TextBlock> column) {
      columns.push_back(std::move(column));
      return true;
    });
    assert(!shortColumn.layoutFailed() && columns.size() == 2);
    assert(columns[0]->getWords().size() == 2 && columns[1]->getWords()[1] == "」");

    ParsedText periodThenQuote(false, style);
    for (int i = 0; i < 19; ++i) periodThenQuote.addWord("あ", EpdFontFamily::REGULAR);
    periodThenQuote.addWord("。", EpdFontFamily::REGULAR);
    periodThenQuote.addWord("」", EpdFontFamily::REGULAR);
    periodThenQuote.addWord("え", EpdFontFamily::REGULAR);
    columns.clear();
    periodThenQuote.layoutVerticalColumns(renderer, 1, 400, [&](std::unique_ptr<TextBlock> column) {
      columns.push_back(std::move(column));
      return true;
    });
    assert(!periodThenQuote.layoutFailed() && columns.size() == 2);
    assert(columns[0]->getWords().size() == 21 && columns[1]->getWords().size() == 1);
    assert(columns[0]->getWords()[19] == "。" && columns[0]->getWords()[20] == "」");
    assert(columns[1]->getWords().front() == "え");

    ParsedText mixedStyle(false, style);
    for (int i = 0; i < 20; ++i) mixedStyle.addWord("あ", EpdFontFamily::REGULAR);
    mixedStyle.addWord("」", EpdFontFamily::BOLD);
    mixedStyle.addWord("え", EpdFontFamily::REGULAR);
    columns.clear();
    mixedStyle.layoutVerticalColumns(renderer, 1, 400, [&](std::unique_ptr<TextBlock> column) {
      columns.push_back(std::move(column));
      return true;
    });
    assert(!mixedStyle.layoutFailed() && columns.size() == 2);
    assert(columns[0]->getWords().size() == 19 && columns[1]->getWords()[1] == "」");
  }
  ESP = {};
  assert(!LayoutMemory::admit(std::numeric_limits<size_t>::max(), "overflow"));
  assert(LayoutMemory::multiply(std::numeric_limits<size_t>::max(), 2) == std::numeric_limits<size_t>::max());
  std::cout << "Layout admission, transactional consumption, sparse images, ruby/emphasis, kinsoku, prewarm and "
               "partial flush: PASS\n";
}
