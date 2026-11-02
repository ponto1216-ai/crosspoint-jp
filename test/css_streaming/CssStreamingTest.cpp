#include <Arduino.h>
#include <Epub/css/CssParser.h>
#include <Epub/css/CssSelectorUsage.h>

#include <cassert>
#include <iostream>
FsFile source(const std::string& s) {
  FsFile f;
  f.bytes = std::make_shared<std::vector<uint8_t>>(s.begin(), s.end());
  return f;
}
int main() {
  const std::string a =
      "p, .a { font-weight: bold; margin-left: 2em; } .a { text-align: right; text-emphasis: filled sesame; }";
  const std::string b = ".a { font-weight: normal; font-size: 120%; text-emphasis: none; } p { margin-left: 3em; }";
  CssParser reference("/reference"), streamed("/stream");
  for (const auto& text : {a, b}) {
    auto f = source(text);
    assert(reference.loadFromStream(f));
  }
  assert(streamed.beginCacheWrite());
  assert(!streamed.hasCache());
  for (const auto& text : {a, b}) {
    auto f = source(text);
    assert(streamed.loadFromStream(f));
    assert(streamed.ruleCount() == 0);
  }
  assert(streamed.finishCacheWrite() && streamed.validateCache());
  assert(streamed.loadFromCache(0, nullptr, true));
  assert(streamed.ruleCount() == reference.ruleCount());
  for (const auto& cls : {"", "a"}) {
    auto expected = reference.resolveStyle("p", cls), actual = streamed.resolveStyle("p", cls);
    assert(expected.fontWeight == actual.fontWeight && expected.textAlign == actual.textAlign);
    assert(expected.marginLeft.value == actual.marginLeft.value && expected.marginLeft.unit == actual.marginLeft.unit);
    assert(expected.fontSize.value == actual.fontSize.value && expected.fontSize.unit == actual.fontSize.unit);
    assert(expected.fontSizeDefined == actual.fontSizeDefined);
    assert(expected.emphasis == actual.emphasis && expected.emphasisDefined == actual.emphasisDefined);
  }
  CssParser ordered("/ordered");
  auto orderCss = source(".a { font-size: 1.5em; } .b { font-size: 0.75em; } "
                         ".a { font-weight: bold; }");
  assert(ordered.loadFromStream(orderCss));
  assert(ordered.saveToCache());
  ordered.clear();
  assert(ordered.loadFromCache(0, nullptr, true));
  for (const auto& classes : {"a b", "b a"}) {
    const auto style = ordered.resolveStyle("p", classes);
    assert(style.hasFontSize() && style.fontSize.value == 0.75f);
    assert(style.fontWeight == CssFontWeight::Bold);
  }
  CssParser breaks("/breaks");
  auto breakCss = source(".forced { page-break-before: always; break-after: page; } "
                         ".cancel { page-break-before: auto; break-after: avoid; }");
  assert(breaks.loadFromStream(breakCss) && breaks.saveToCache());
  breaks.clear();
  assert(breaks.loadFromCache(0, nullptr, true));
  const auto forced = breaks.resolveStyle("p", "forced");
  assert(forced.pageBreakBeforeDefined && forced.pageBreakBefore);
  assert(forced.pageBreakAfterDefined && forced.pageBreakAfter);
  const auto cancelled = breaks.resolveStyle("p", "forced cancel");
  assert(cancelled.pageBreakBeforeDefined && !cancelled.pageBreakBefore);
  assert(cancelled.pageBreakAfterDefined && !cancelled.pageBreakAfter);
  // More records than the old in-memory map limit: retain only the two used selectors on load.
  CssParser large("/large");
  assert(large.beginCacheWrite());
  for (int i = 0; i < 2200; ++i) {
    auto f = source(".unused" + std::to_string(i) + " { margin-left: 2em; }");
    assert(large.loadFromStream(f) && large.empty());
  }
  auto f = source(a);
  assert(large.loadFromStream(f));
  f = source(b);
  assert(large.loadFromStream(f));
  assert(large.finishCacheWrite());
  auto html = source("<html><body><p class=\"a\">text</p></body></html>");
  Storage.files["/chapter"] = html.bytes;
  CssSelectorUsage usage;
  assert(usage.scanHtmlFile("/chapter"));
  assert(large.loadFromCache(0, &usage, true) && large.ruleCount() == 2);
  assert(large.resolveStyle("p", "a").fontWeight == reference.resolveStyle("p", "a").fontWeight);
  // Abort must keep an existing good cache intact.
  const auto old = Storage.files;
  assert(streamed.beginCacheWrite());
  ESP = {};
  ESP.failAt = 0;
  f = source(a);
  assert(!streamed.loadFromStream(f));
  assert(!streamed.finishCacheWrite());
  ESP = {};
  assert(streamed.validateCache());
  for (const auto& item : Storage.files) assert(item.first.find(".tmp") == std::string::npos);
  CssParser failed("/failed");
  FsFile::writeBudget = 0;
  assert(!failed.beginCacheWrite());
  FsFile::writeBudget = -1;
  assert(!failed.hasCache());
  assert(failed.beginCacheWrite());
  FsFile::writeBudget = 0;
  f = source(a);
  assert(!failed.loadFromStream(f));
  assert(!failed.finishCacheWrite());
  FsFile::writeBudget = -1;
  assert(!failed.hasCache());
  assert(failed.beginCacheWrite());
  f = source(a);
  assert(failed.loadFromStream(f));
  FsFile::seekFails = true;
  assert(!failed.finishCacheWrite());
  FsFile::seekFails = false;
  assert(!failed.hasCache());
  assert(failed.beginCacheWrite());
  f = source(a);
  assert(failed.loadFromStream(f));
  Storage.renameFails = true;
  assert(!failed.finishCacheWrite());
  Storage.renameFails = false;
  assert(!failed.hasCache());
  {
    CssParser abandoned("/abandoned");
    assert(abandoned.beginCacheWrite());
  }
  for (const auto& item : Storage.files) assert(item.first.find(".tmp") == std::string::npos);
  assert(failed.beginCacheWrite() && failed.finishCacheWrite());
  assert(failed.loadFromCache(0, nullptr, true) && failed.empty());
  std::cout << "PASS: source-order cascade, multi-file duplicates, 2200 unused selectors with zero map growth, "
               "filtered restore, empty CSS, low heap and publication failures\n";
}
