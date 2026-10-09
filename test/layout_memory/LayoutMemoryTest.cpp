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
  ESP = {};
  assert(!LayoutMemory::admit(std::numeric_limits<size_t>::max(), "overflow"));
  assert(LayoutMemory::multiply(std::numeric_limits<size_t>::max(), 2) == std::numeric_limits<size_t>::max());
  std::cout << "Layout admission, transactional consumption, sparse images, ruby/emphasis, kinsoku, prewarm and "
               "partial flush: PASS\n";
}
