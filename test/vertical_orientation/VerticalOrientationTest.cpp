#include <VerticalTextUtils.h>

#include <cassert>

using VerticalTextUtils::UaxVerticalOrientation;

int main() {
  // UAX #50 examples that are easy to mistake for a rendering bug.
  assert(VerticalTextUtils::getUaxVerticalOrientation(0x2190) == UaxVerticalOrientation::Rotated);  // ←
  assert(VerticalTextUtils::getUaxVerticalOrientation(0x03B1) == UaxVerticalOrientation::Rotated);  // α
  assert(VerticalTextUtils::getUaxVerticalOrientation(0x0410) == UaxVerticalOrientation::Rotated);  // А
  assert(VerticalTextUtils::getUaxVerticalOrientation(0x00C5) == UaxVerticalOrientation::Rotated);  // Å

  assert(VerticalTextUtils::isUaxUprightInVertical(0x2460));  // ①
  assert(VerticalTextUtils::isUaxUprightInVertical(0x2160));  // Ⅰ
  assert(VerticalTextUtils::isUaxUprightInVertical(0x2103));  // ℃
  assert(VerticalTextUtils::isUaxUprightInVertical(0x2113));  // ℓ
  assert(VerticalTextUtils::isUaxUprightInVertical(0x2116));  // №

  assert(VerticalTextUtils::getUaxVerticalOrientation(0x3001) == UaxVerticalOrientation::TransformedUpright);  // 、
  assert(VerticalTextUtils::getUaxVerticalOrientation(0x30FC) == UaxVerticalOrientation::TransformedRotated);  // ー
  assert(VerticalTextUtils::getUaxVerticalOrientation(0xFF70) == UaxVerticalOrientation::Rotated);             // ｰ

  // Tr punctuation and Japanese curly quotes must follow the same single-cell
  // path through parsing, layout, and rendering.
  assert(VerticalTextUtils::isVerticalGlyphCell(0x300C));  // 「 (Tr)
  assert(VerticalTextUtils::isVerticalGlyphCell(0x300D));  // 」 (Tr)
  assert(VerticalTextUtils::isVerticalGlyphCell(0x301D));  // 〝 (Tr)
  assert(VerticalTextUtils::isVerticalGlyphCell(0x201C));  // “ (R, Japanese quote exception)
  assert(VerticalTextUtils::isVerticalGlyphCell(0x201D));  // ” (R, Japanese quote exception)
  assert(VerticalTextUtils::getUaxVerticalOrientation(0x2026) == UaxVerticalOrientation::Rotated);
  assert(VerticalTextUtils::isVerticalGlyphCell(0x2025));  // ‥ uses one vertical glyph cell
  assert(VerticalTextUtils::isVerticalGlyphCell(0x2026));  // … uses one vertical glyph cell
  assert(!VerticalTextUtils::isVerticalGlyphCell('A'));    // ordinary R text remains sideways
  assert(VerticalTextUtils::isFullwidthLatinLetter(0xFF21));
  assert(VerticalTextUtils::isFullwidthLatinLetter(0xFF41));
  assert(!VerticalTextUtils::isFullwidthLatinLetter('A'));
  assert(VerticalTextUtils::startsWithAsciiLatinLetter("ABC"));
  assert(VerticalTextUtils::startsWithAsciiLatinLetter("abc"));
  assert(!VerticalTextUtils::startsWithAsciiLatinLetter("@"));
  assert(!VerticalTextUtils::startsWithAsciiLatinLetter("-ABC"));
  assert(VerticalTextUtils::startsWithAsciiPunctuation("+"));
  assert(VerticalTextUtils::startsWithAsciiPunctuation("@"));
  assert(!VerticalTextUtils::startsWithAsciiPunctuation("ABC"));
  assert(VerticalTextUtils::sidewaysTextVerticalOffset("BIZUDGothic_14.cpfont", "+", 26) == 0);
  assert(VerticalTextUtils::sidewaysTextVerticalOffset("BIZUDGothic_14.cpfont", "ABC", 26) == 8);
  assert(VerticalTextUtils::sidewaysTextVerticalOffset("NotoSerifJp_14.cpfont", "+", 34) == 11);
  assert(VerticalTextUtils::sidewaysTextVerticalOffset("NotoSerifJp_14.cpfont", "ABC", 34) == 0);
  assert(VerticalTextUtils::sidewaysTextVerticalOffset("ZenMaruGothic_14.cpfont", "@", 34) == 11);

  const auto* openingQuote = VerticalTextUtils::getVerticalPunctuationOffset(0x201C);
  const auto* closingQuote = VerticalTextUtils::getVerticalPunctuationOffset(0x201D);
  assert(openingQuote != nullptr && openingQuote->rotate && openingQuote->dxEighths > 0 && openingQuote->dyEighths > 0);
  assert(closingQuote != nullptr && closingQuote->rotate && closingQuote->dxEighths < 0);
  const auto* waveDash = VerticalTextUtils::getVerticalPunctuationOffset(0x301C);
  assert(waveDash != nullptr && waveDash->rotate && waveDash->dxEighths == 0);

  using VerticalTextUtils::TateChuYokoKind;
  assert(VerticalTextUtils::classifyTateChuYoko("12") == TateChuYokoKind::DoubleDigit);
  assert(VerticalTextUtils::classifyTateChuYoko("123") == TateChuYokoKind::None);
  assert(VerticalTextUtils::classifyTateChuYoko("123", 3) == TateChuYokoKind::TripleDigit);
  assert(VerticalTextUtils::classifyTateChuYoko("1234", 3) == TateChuYokoKind::None);
  assert(VerticalTextUtils::isSidewaysDigitAfterEllipsis("…", "5"));
  assert(VerticalTextUtils::isSidewaysDigitAfterEllipsis("‥", "6"));
  assert(!VerticalTextUtils::isSidewaysDigitAfterEllipsis("…", "12"));
  assert(!VerticalTextUtils::isSidewaysDigitAfterEllipsis("あ", "5"));
  assert(VerticalTextUtils::numericDashRunBytes("1―2—3–4…5", sizeof("1―2—3–4…5") - 1) ==
         sizeof("1―2—3–4") - 1);
  assert(VerticalTextUtils::numericDashRunBytes("1―23", sizeof("1―23") - 1) == 0);
  assert(VerticalTextUtils::ellipsisDigitTightening("NotoSerifJp_14.cpfont", 28) == 0);
  assert(VerticalTextUtils::ellipsisDigitTightening("BIZUDMincho_14.cpfont", 28) == 7);
}
