#pragma once

#include <cstdint>

#include "VerticalOrientationData.h"

namespace VerticalTextUtils {

// UAX #50's codepoint-level default is available alongside the current
// renderer behavior during the migration. New vertical rules must be checked
// against isUaxUprightInVertical() rather than adding ad-hoc block ranges.

// Character behavior in vertical text layout
enum class VerticalBehavior : uint8_t {
  Upright,      // CJK ideographs, kana - draw normally, advance downward
  Sideways,     // Latin letters, 3+ digit numbers - rotate 90 CW
  TateChuYoko,  // 1-2 digit numbers - horizontal-in-vertical
};

// Punctuation offset for vertical text (ratio of character size, in 1/8 units)
struct PunctuationOffset {
  uint32_t codepoint;
  int8_t dxEighths;  // horizontal offset in 1/8 of charWidth
  int8_t dyEighths;  // vertical offset in 1/8 of charHeight
  bool rotate;       // true = rotate 90 CW (e.g. long vowel mark)
};

// CJK punctuation and brackets that need 90 CW rotation in vertical text.
// Horizontal font glyphs are designed for horizontal layout; rotating them
// naturally transforms their position to the correct vertical placement
// (e.g. 。at bottom-left becomes upper-right after rotation).
// dx/dyEighths are post-rotation fine-tuning offsets (usually 0).
static constexpr PunctuationOffset VERTICAL_PUNCTUATION[] = {
    // Punctuation - rotate to reposition from horizontal to vertical placement
    {0x3001, 0, 0, true},  // 、 ideographic comma
    {0x3002, 0, 0, true},  // 。 ideographic period
    {0xFF0C, 0, 0, true},  // ， fullwidth comma
    {0xFF0E, 0, 0, true},  // ． fullwidth period
    {0xFF1A, 0, 0, true},  // ： fullwidth colon
    {0xFF1B, 0, 0, true},  // ； fullwidth semicolon
    {0xFF1C, 0, 0, true},  // fullwidth less-than sign
    {0xFF1E, 0, 0, true},  // fullwidth greater-than sign
    {0xFF61, 0, 0, true},  // ｡ halfwidth ideographic period
    {0xFF64, 0, 0, true},  // ､ halfwidth ideographic comma
    {0xFF65, 0, -2, true}, // ･ halfwidth katakana middle dot (raise in cell)
    // Brackets - rotate so opening/closing direction matches vertical flow
    {0x300C, 0, 0, true},  // 「 left corner bracket
    {0x300D, 0, 0, true},  // 」 right corner bracket
    {0xFF62, 0, 0, true},  // ｢ halfwidth left corner bracket
    {0xFF63, 0, 0, true},  // ｣ halfwidth right corner bracket
    {0x300E, 0, 0, true},  // 『 left white corner bracket
    {0x300F, 0, 0, true},  // 』 right white corner bracket
    {0x3010, 0, 0, true},  // 【 left black lenticular bracket
    {0x3011, 0, 0, true},  // 】 right black lenticular bracket
    {0xFF08, 0, 0, true},  // （ fullwidth left paren
    {0xFF09, 0, 0, true},  // ） fullwidth right paren
    {0x3008, 0, 0, true},  // 〈 left angle bracket
    {0x3009, 0, 0, true},  // 〉 right angle bracket
    {0x300A, 0, 0, true},  // 《 left double angle bracket
    {0x300B, 0, 0, true},  // 》 right double angle bracket
    {0x3014, 0, 0, true},  // 〔 left tortoise shell bracket
    {0x3015, 0, 0, true},  // 〕 right tortoise shell bracket
    // Long marks - rotate to vertical orientation
    {0x30FC, 0, 0, true},  // ー katakana long vowel mark
    {0xFF70, 0, 0, true},  // ｰ halfwidth katakana-hiragana prolonged sound mark
    {0x2014, 0, 0, true},  // — em dash
    {0x2015, 0, 0, true},  // ― horizontal bar
    {0x2026, 0, 0, true},  // … ellipsis
    {0x301C, -1, 0, true}, // 〜 wave dash: center the rotated fallback glyph
    {0xFF0D, 0, 0, true},  // － fullwidth hyphen-minus
    {0xFF5E, 0, 0, true},  // ～ fullwidth tilde
};
static constexpr int VERTICAL_PUNCTUATION_COUNT = sizeof(VERTICAL_PUNCTUATION) / sizeof(VERTICAL_PUNCTUATION[0]);

// Look up punctuation offset. Returns nullptr if no special handling needed.
inline const PunctuationOffset* getVerticalPunctuationOffset(uint32_t cp) {
  for (int i = 0; i < VERTICAL_PUNCTUATION_COUNT; i++) {
    if (VERTICAL_PUNCTUATION[i].codepoint == cp) return &VERTICAL_PUNCTUATION[i];
  }
  return nullptr;
}

// Halfwidth katakana are narrow horizontal glyphs, but in vertical Japanese
// text each one occupies a normal character cell.
inline bool isHalfwidthKatakana(uint32_t cp) {
  // U+FF65 (･) and U+FF70 (ｰ) are punctuation/long marks. They use the
  // rotated punctuation path below; only actual katakana keep kana metrics.
  return (cp >= 0xFF66 && cp <= 0xFF6F) || (cp >= 0xFF71 && cp <= 0xFF9D);
}

// Keep a two-character ASCII !/? sequence in one vertical cell. A single
// mark and longer runs deliberately remain sideways, matching Japanese
// vertical typesetting conventions.
// Circled digits and related enclosed alphanumerics are conventionally kept
// upright in Japanese vertical text. They are narrow glyphs, but use a full
// Japanese character cell for line breaking and spacing.
inline bool isEnclosedAlphanumeric(uint32_t cp) {
  return cp >= 0x2460 && cp <= 0x24FF;
}

inline bool isTateChuYokoPunctuationPair(const char* text) {
  return text != nullptr && (text[0] == '!' || text[0] == '?') && (text[1] == '!' || text[1] == '?') &&
         text[2] == '\0';
}

// Short ASCII runs that share one Japanese vertical character cell.
// Keep this classification in one place: the parser stores it for layout and
// TextBlock repeats it when drawing restored section-cache entries.
enum class TateChuYokoKind : uint8_t {
  None,
  SingleDigit,
  DoubleDigit,
  PunctuationPair,
};

inline TateChuYokoKind classifyTateChuYoko(const char* text) {
  if (text == nullptr || *text == '\0') return TateChuYokoKind::None;

  int digitCount = 0;
  for (const char* p = text; *p; ++p) {
    if (*p < '0' || *p > '9') {
      return isTateChuYokoPunctuationPair(text) ? TateChuYokoKind::PunctuationPair : TateChuYokoKind::None;
    }
    ++digitCount;
  }

  if (digitCount == 1) return TateChuYokoKind::SingleDigit;
  if (digitCount == 2) return TateChuYokoKind::DoubleDigit;
  return TateChuYokoKind::None;
}

// A ruby annotation containing Latin letters is conventionally rotated as a
// whole phrase in vertical Japanese text, rather than stacking its letters.
inline bool isAsciiAlphabeticWord(const char* text) {
  if (text == nullptr || *text == '\0') return false;
  for (const char* p = text; *p; ++p) {
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
      return true;
    }
  }
  return false;
}

// Determine if a codepoint should be drawn upright in vertical text.
// CJK ideographs, kana, CJK symbols, fullwidth forms, etc.
inline bool isUprightInVertical(uint32_t cp) {
  return isUaxUprightInVertical(cp);
}

// Tr characters require a vertical presentation when the font has one, but
// still reserve a single Japanese cell when Yomuka falls back to rotation.
inline bool isTransformedRotatedInVertical(const uint32_t cp) {
  return getUaxVerticalOrientation(cp) == UaxVerticalOrientation::TransformedRotated;
}

// Should this codepoint use the OpenType 'vert' substitute glyph?
// Returns true only for punctuation, brackets, and long marks that need
// a different glyph shape in vertical text. Kana and ideographs are excluded
// because their vert variants differ only in metrics (designed for use with
// a full shaping engine), and bitmap-only substitution looks wrong.
inline bool shouldUseVertGlyph(uint32_t cp) {
  // CJK punctuation and brackets (3000-303F), excluding ideographs like 〆(3006)
  if (cp == 0x3001 || cp == 0x3002) return true;  // 、。
  if (cp >= 0x3008 && cp <= 0x3011) return true;  // 〈〉《》「」『』【】
  if (cp >= 0x3014 && cp <= 0x301B) return true;  // 〔〕〖〗〘〙〚〛
  if (cp >= 0x301D && cp <= 0x301F) return true;  // 〝〞〟
  if (cp == 0x301C) return true;                  // 〜
  // Fullwidth punctuation and brackets
  // Fullwidth !/? use the rotated normal glyph below. Some SD fonts carry
  // horizontal-looking vert alternates for these two punctuation marks.
  if (cp == 0xFF08 || cp == 0xFF09) return true;  // （）
  if (cp == 0xFF61 || cp == 0xFF64) return true;  // ｡､
  if (cp == 0xFF0C || cp == 0xFF0E) return true;  // ，．
  if (cp == 0xFF1A || cp == 0xFF1B) return true;  // ：；
  if (cp == 0xFF3B || cp == 0xFF3D) return true;  // ［］
  if (cp == 0xFF5B || cp == 0xFF5D) return true;  // ｛｝
  if (cp == 0xFF0D) return true;                  // －
  if (cp == 0xFF5E) return true;                  // ～
  if (cp == 0xFF1C || cp == 0xFF1E) return true;
  // Long marks and dashes
  if (cp == 0x30FC) return true;                  // ー
  if (cp == 0x2014 || cp == 0x2015) return true;  // —―
  if (cp == 0x2025 || cp == 0x2026) return true;  // ‥…
  if (cp == 0x22EF) return true;                  // ⋯
  return false;
}

// Kinsoku (禁則) processing for vertical text column breaks.
// Returns true if this codepoint must NOT appear at the start of a column.
inline constexpr bool isSmallKana(uint32_t cp) {
  switch (cp) {
    case 0x3041:  // ぁ
    case 0x3043:  // ぃ
    case 0x3045:  // ぅ
    case 0x3047:  // ぇ
    case 0x3049:  // ぉ
    case 0x3063:  // っ
    case 0x3083:  // ゃ
    case 0x3085:  // ゅ
    case 0x3087:  // ょ
    case 0x308E:  // ゎ
    case 0x3095:  // ゕ
    case 0x3096:  // ゖ
    case 0x30A1:  // ァ
    case 0x30A3:  // ィ
    case 0x30A5:  // ゥ
    case 0x30A7:  // ェ
    case 0x30A9:  // ォ
    case 0x30C3:  // ッ
    case 0x30E3:  // ャ
    case 0x30E5:  // ュ
    case 0x30E7:  // ョ
    case 0x30EE:  // ヮ
    case 0x30F5:  // ヵ
    case 0x30F6:  // ヶ
      return true;
    default:
      return cp >= 0x31F0 && cp <= 0x31FF;  // Ainu small katakana
  }
}

inline bool isKinsokuHead(uint32_t cp) {
  // Closing brackets and punctuation (行頭禁止)
  if (cp == 0x3001 || cp == 0x3002) return true;                                  // 、。
  if (cp == 0xFF61 || cp == 0xFF63 || cp == 0xFF64) return true;                  // ｡｣､
  if (cp == 0x300D || cp == 0x300F || cp == 0x3011) return true;                  // 」』】
  if (cp == 0x3015 || cp == 0x3017 || cp == 0x3019 || cp == 0x301B) return true;  // 〕〗〙〛
  if (cp == 0xFF09 || cp == 0xFF3D || cp == 0xFF5D) return true;                  // ）］｝
  if (cp == 0xFF0C || cp == 0xFF0E) return true;                                  // ，．
  if (cp == 0xFF01 || cp == 0xFF1F) return true;                                  // ！？
  if (cp == 0xFF1A || cp == 0xFF1B) return true;                                  // ：；
  if (cp == 0x3009 || cp == 0x300B) return true;                                  // 〉》
  // Small kana (行頭禁止). Keep this shared with the complete small-kana
  // list so ゎ・ヮ・ゕ・ゖ・ヵ・ヶ and Ainu small katakana cannot be omitted.
  if (isSmallKana(cp)) return true;
  if (cp == 0x30FC || cp == 0xFF70) return true;                                                  // ーｰ
  return false;
}

// Returns true if this codepoint must NOT appear at the end of a column.
inline bool isKinsokuTail(uint32_t cp) {
  // Opening brackets (行末禁止)
  if (cp == 0x300C || cp == 0x300E || cp == 0x3010) return true;                  // 「『【
  if (cp == 0xFF62) return true;                                                    // ｢
  if (cp == 0x3014 || cp == 0x3016 || cp == 0x3018 || cp == 0x301A) return true;  // 〔〖〘〚
  if (cp == 0xFF08 || cp == 0xFF3B || cp == 0xFF5B) return true;                  // （［｛
  if (cp == 0x3008 || cp == 0x300A) return true;                                  // 〈《
  return false;
}

}  // namespace VerticalTextUtils
