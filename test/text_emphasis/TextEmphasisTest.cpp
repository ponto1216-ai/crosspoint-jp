#include <GfxRenderer.h>

#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>

#include "Epub/blocks/ImageBlock.h"
#include "Epub/blocks/TextBlock.h"
#include "Epub/css/CssParser.h"

// Inline images are outside these text-only fixtures.
ImageBlock::ImageBlock(const std::string& path, int16_t w, int16_t h) : imagePath(path), width(w), height(h) {}
void ImageBlock::render(GfxRenderer&, int, int) { assert(false); }

TextBlock block(std::string text, TextEmphasis mark, bool vertical = false) {
  return TextBlock({text}, {0}, {EpdFontFamily::REGULAR}, {}, {0}, vertical, {}, {}, {mark});
}
int ink(const GfxRenderer& r) { return std::count(r.pixels.begin(), r.pixels.end(), 0); }
void rubySpanRegression() {
  using E = TextEmphasis;
  TextBlock::rubyFontId = 2;
  for (bool vertical : {false, true})
    for (bool bizud : {false, true}) {
      const auto make = [&](std::vector<TextEmphasis> marks, bool ruby = true) {
        return TextBlock(
            {"日", "本", "語"}, vertical ? std::vector<int16_t>{0, 0, 0} : std::vector<int16_t>{0, 25, 50},
            {EpdFontFamily::REGULAR, EpdFontFamily::REGULAR, EpdFontFamily::REGULAR}, {}, {0, 25, 50}, vertical,
            ruby ? std::vector<std::string>{"にほん", std::string(1, TextBlock::RUBY_CONTINUATION_MARKER), ""}
                 : std::vector<std::string>{},
            {}, marks);
      };
      auto plain = make({});
      auto separate = make({E::None, E::None, E::FilledDot});
      auto overlap = make({E::None, E::FilledDot, E::None});
      auto first = make({E::FilledDot});
      auto marksOnly = make({E::FilledDot}, false);
      assert(!plain.rubyBaseHasEmphasis(0) && !separate.rubyBaseHasEmphasis(0));
      assert(overlap.rubyBaseHasEmphasis(0) && first.rubyBaseHasEmphasis(0));
      assert(!overlap.rubyBaseHasEmphasis(1) && !overlap.rubyBaseHasEmphasis(99));
      const auto render = [&](const TextBlock& b, int x = 100, int y = 70, int ox = 0, int oy = 0) {
        GfxRenderer r;
        if (bizud) {
          r.bodyHeight = 29;
          r.rubyHeight = 17;
        }
        b.render(r, 1, x, y, 900, 1000, 0, 0, ox, oy);
        return r;
      };
      const auto rubyDraw = [](const GfxRenderer& r) {
        for (const auto& d : r.draws)
          if (d.font == 2) return d;
        assert(false);
        return GfxRenderer::Draw{};
      };
      auto p = render(plain), s = render(separate), o = render(overlap), m = render(marksOnly);
      auto pd = rubyDraw(p), sd = rubyDraw(s), od = rubyDraw(o);
      assert(pd.x == sd.x && pd.y == sd.y);
      assert(vertical ? od.x > pd.x : od.y < pd.y);
      if (vertical) assert(od.x == 100 + (bizud ? 38 : 28));
      for (const auto& d : m.draws) assert(d.font != 2);
      assert(ink(m) > 0);
      assert(separate.annotationRightOverflow(p, 1, p.bodyHeight) ==
             std::max(plain.annotationRightOverflow(p, 1, p.bodyHeight),
                      marksOnly.annotationRightOverflow(p, 1, p.bodyHeight)));
      assert(separate.annotationTopInset(p, 1) ==
             std::max(plain.annotationTopInset(p, 1), marksOnly.annotationTopInset(p, 1)));
      assert(overlap.annotationRightOverflow(p, 1, p.bodyHeight) >
             separate.annotationRightOverflow(p, 1, p.bodyHeight));
      assert(overlap.annotationTopInset(p, 1) > separate.annotationTopInset(p, 1));
      auto shifted = rubyDraw(render(separate, 100, 70, 3, 4));
      assert(shifted.x == sd.x + 3 && shifted.y == sd.y + 4);
      auto edge = rubyDraw(render(separate, 895, 0));
      assert(edge.x <= 900 - p.rubyHeight && edge.y >= 2);
      if (!vertical)
        assert(pd.x == 100 + (25 + p.bodyHeight - 3 * p.bodyHeight) / 2);  // Whole two-token base centering.
    }
  TextBlock punctuation({"。"}, {0}, {EpdFontFamily::REGULAR}, {}, {}, false, {"まる"}, {}, {E::FilledDot});
  assert(!punctuation.rubyBaseHasEmphasis(0));
  TextBlock::rubyFontId = 0;
}
int main(int argc, char** argv) {
  rubySpanRegression();
  using E = TextEmphasis;
  E e = E::None;
  assert(textEmphasis::parse("open sesame", e) && e == E::OpenSesame);
  assert(textEmphasis::parse("SESAME FILLED", e) && e == E::FilledSesame);
  assert(textEmphasis::parse("none", e) && e == E::None);
  assert(textEmphasis::parse("open", e) && textEmphasis::codepoint(e, true) == 0xfe46 &&
         textEmphasis::codepoint(e, false) == 0x25e6);
  assert(textEmphasis::parse("filled sesame black", e, true) && e == E::FilledSesame);
  for (auto s : {"", "invalid", "inherit", "none dot", "open filled", "dot circle", "'x'"})
    assert(!textEmphasis::parse(s, e));
  CssStyle parent, child;
  parent.emphasis = E::FilledSesame;
  parent.emphasisDefined = true;
  child.emphasis = E::None;
  child.emphasisDefined = true;
  parent.applyOver(child);
  assert(parent.emphasis == E::None && parent.anySet());
  parent.reset();
  assert(!parent.emphasisDefined);
  for (auto name : {"text-emphasis", "text-emphasis-style", "-epub-text-emphasis", "-epub-text-emphasis-style",
                    "-webkit-text-emphasis", "-webkit-text-emphasis-style"}) {
    const auto style = CssParser::parseInlineStyle(std::string(name) + ":open sesame !important");
    assert(style.emphasisDefined && style.emphasis == E::OpenSesame);
  }
  FsFile css;
  Storage.openFileForWrite("TEST", "/source", css);
  const std::string rules =
      ".mark{text-emphasis:filled sesame;color:black}.off{text-emphasis:none}.other{width:40%;font-weight:bold}";
  css.write(reinterpret_cast<const uint8_t*>(rules.data()), rules.size());
  CssParser parser("/cache");
  parser.setCacheSourceFingerprint(123);
  assert(parser.loadFromStream(css));
  assert(parser.saveToCache());
  CssParser restored("/cache");
  restored.setCacheSourceFingerprint(123);
  assert(restored.loadFromCache());
  assert(restored.resolveStyle("span", "mark").emphasis == E::FilledSesame);
  assert(restored.resolveStyle("span", "off").emphasisDefined &&
         restored.resolveStyle("span", "off").emphasis == E::None);
  assert(restored.resolveStyle("span", "other").imageWidth.value == 40);
  std::string cachePath;
  for (const auto& item : Storage.files)
    if (item.first.find("/cache/") == 0) cachePath = item.first;
  assert(!cachePath.empty());
  const auto good = *Storage.files[cachePath];
  for (size_t length = 0; length < good.size(); ++length) {
    Storage.files[cachePath] = std::make_shared<std::vector<uint8_t>>(good.begin(), good.begin() + length);
    CssParser truncated("/cache");
    truncated.setCacheSourceFingerprint(123);
    assert(!truncated.loadFromCache());
  }
  Storage.files[cachePath] = std::make_shared<std::vector<uint8_t>>(good);
  (*Storage.files[cachePath])[0] = 7;
  assert(!restored.loadFromCache());
  for (auto cp : {0x20, 0x3000, 0x3001, 0x3002, 0x3099, 0xfe0f, 0xe0100, 0xfffc}) assert(!textEmphasis::eligible(cp));
  for (auto cp : {0x3005, 0x4e00, 0x3042, 'A' + 0, '3' + 0}) assert(textEmphasis::eligible(cp));
  GfxRenderer missing;
  block("A", E::FilledSesame).renderEmphasis(missing, 1, 20, 30);
  assert(ink(missing) > 0);
  assert(missing.fonts[1].probes[0xfe45] == 1 && missing.fonts[1].probes[0x2022] == 1);
  GfxRenderer open;
  block("A", E::OpenSesame).renderEmphasis(open, 1, 20, 30);
  assert(ink(open) > 0 && ink(open) < ink(missing));
  GfxRenderer fallback;
  fallback.fonts[1].glyphs[0x2022] = {};
  block("AAAA", E::FilledSesame).renderEmphasis(fallback, 1, 20, 30);
  assert(ink(fallback) > 0 && fallback.fonts[1].probes[0xfe45] == 1 && fallback.fonts[1].probes[0x2022] == 1);
  GfxRenderer exact;
  exact.fonts[1].glyphs[0xfe45] = {};
  block("A", E::FilledSesame).renderEmphasis(exact, 1, 20, 30);
  assert(ink(exact) > 0 && exact.fonts[1].probes[0x2022] == 0);
  auto reusable = block("A", E::FilledSesame);
  GfxRenderer changedFont;
  reusable.renderEmphasis(changedFont, 1, 20, 30);
  const auto before = changedFont.pixels;
  std::fill(changedFont.pixels.begin(), changedFont.pixels.end(), 255);
  changedFont.fonts[1].glyphs[0xfe45] = {};
  reusable.renderEmphasis(changedFont, 1, 20, 30);
  assert(before != changedFont.pixels);
  TextBlock withRuby({"A"}, {0}, {EpdFontFamily::REGULAR}, {}, {0}, false, {"ruby"}, {}, {E::FilledDot});
  TextBlock::rubyFontId = 2;
  assert(withRuby.annotationTopInset(missing, 1) > block("A", E::FilledDot).annotationTopInset(missing, 1));
  TextBlock::rubyFontId = 0;
  GfxRenderer rubyOff;
  withRuby.renderEmphasis(rubyOff, 1, 20, 30);
  assert(ink(rubyOff) > 0);
  GfxRenderer longRun;
  block(std::string(52, 'A'), E::FilledDot).renderEmphasis(longRun, 1, 20, 30);
  assert(ink(longRun) == ink(missing) * 52);
  GfxRenderer punct;
  block(" ,.!", E::FilledDot).renderEmphasis(punct, 1, 20, 30);
  assert(ink(punct) == 0);
  GfxRenderer scan;
  scan.cache.scanning = true;
  block("A", E::FilledDot).renderEmphasis(scan, 1, 20, 30);
  assert(ink(scan) == 0);
  auto v = block("A", E::FilledDot, true);
  assert(v.annotationRightOverflow(missing, 1, 25) > 0 && v.annotationTopInset(missing, 1) > 0);
  GfxRenderer vertical;
  v.renderEmphasis(vertical, 1, 20, 30);
  assert(ink(vertical) > 0);
  GfxRenderer verticalRun;
  block("ABC", E::FilledDot, true).renderEmphasis(verticalRun, 1, 20, 30);
  assert(ink(verticalRun) == ink(missing) * 3);
  if (argc > 1) {
    GfxRenderer sheet;
    block("FILLED", E::FilledSesame).renderEmphasis(sheet, 1, 20, 30);
    block("OPEN", E::OpenSesame).renderEmphasis(sheet, 1, 20, 70);
    sheet.fonts[1].glyphs[0x2022] = {};
    block("GLYPH", E::FilledSesame).renderEmphasis(sheet, 1, 20, 110);
    std::ofstream out(argv[1], std::ios::binary);
    out << "P5\n" << GfxRenderer::W << " " << GfxRenderer::H << "\n255\n";
    out.write(reinterpret_cast<const char*>(sheet.pixels.data()), sheet.pixels.size());
  }
  std::cout << "PASS: CSS/cache roundtrip and truncation, inheritance, font fallback, geometric fallback, 52 "
               "characters, punctuation, scan and vertical placement\n";
}
