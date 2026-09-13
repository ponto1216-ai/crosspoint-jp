#pragma once

#include <cstdint>

#include "EpdFont.h"
#include "EpdFontData.h"

class SdCardFont {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 128;
  static constexpr uint8_t MAX_STYLES = 4;
  static constexpr uint8_t KERN_ROW_CACHE_ROWS = 4;

  SdCardFont() = default;
  ~SdCardFont();

  // Load .cpfont file: reads header + intervals into RAM, records file layout offsets.
  // Supports v4 (multi-style) format.
  // Returns true on success.
  bool load(const char* path);

  // Pre-read glyphs needed for the given UTF-8 text from SD card.
  // styleMask: bitmask of styles to prewarm (bit 0=regular, 1=bold, 2=italic, 3=bolditalic).
  // Default 0x0F = all present styles.
  // When metadataOnly=true, only glyph metrics are loaded (no bitmap data).
  // Returns number of glyphs that couldn't be loaded (0 on full success).
  int prewarm(const char* utf8Text, uint8_t styleMask = 0x0F, bool metadataOnly = false);

  // Build a compact advance-only table for layout measurement.
  // Extracts ALL unique codepoints from utf8Text (no MAX_PAGE_GLYPHS cap),
  // batch-reads advanceX from SD, stores in a sorted per-style table.
  // Returns number of codepoints not found in font coverage.
  int buildAdvanceTable(const char* utf8Text, uint8_t styleMask = 0x0F);

  // Look up advanceX for a codepoint from the advance table.
  // Returns the 12.4 fixed-point advance, or 0 if not found.
  uint16_t getAdvance(uint32_t codepoint, uint8_t style) const;

  // Look up an advance-table entry, loading one glyph's metrics on demand when
  // the bounded table does not contain the codepoint.  This keeps layout
  // correct after a long CJK section reaches the advance-table capacity.
  uint16_t getAdvanceOrLoad(uint32_t codepoint, uint8_t style);

  // Like getAdvanceOrLoad(), but distinguishes a missing glyph from a glyph
  // whose advance is legitimately zero (for example, a combining mark).
  bool tryGetAdvanceOrLoad(uint32_t codepoint, uint8_t style, uint16_t& advanceOut);

  // Returns true if advance table is populated for at least one style.
  bool hasAdvanceTable() const;

  // Clear layout-only advance data at a section boundary. The following
  // buildAdvanceTable() calls then grow one shared table for that section.
  void resetAdvanceTable() { clearAdvanceTables(); }

  // Free transient prewarm/advance data for all styles and restore stub
  // EpdFontData. Vertical substitution data stays resident until the font is
  // unloaded so routine cache clears cannot degrade vertical punctuation.
  void clearCache();

  // Free kern/ligature data for all styles (reclaim memory before heavy operations).
  // Data will be lazy-loaded again on next prewarm.
  void freeKernLigatureData();

  // Release rebuildable glyph, kern/ligature, overflow and advance caches
  // while keeping the font registered and usable. The next render faults data
  // back in from SD as needed.
  void releaseResidentCaches();

  // Release optional vertical substitution glyphs. They can be reloaded before
  // rendering, but keeping them resident can prevent the ZIP stream buffer
  // from obtaining a contiguous allocation during section generation.
  void releaseVerticalGlyphs();

  // Returns pointer to the managed EpdFont for a given style.
  // Returns nullptr if the style is not present.
  EpdFont* getEpdFont(uint8_t style = 0);

  // Returns true if the given style is present in this font file.
  bool hasStyle(uint8_t style) const;

  // Resolve requested style bits to the closest present style.
  uint8_t resolveStyle(uint8_t style) const;

  // Resolve every requested style bit through fallback and return the actual
  // styles that need cache/advance preparation.
  uint8_t resolveStyleMask(uint8_t styleMask) const;

  // Number of styles present in this font file.
  uint8_t styleCount() const { return styleCount_; }

  // Look up vertical substitute glyph for a codepoint.
  // Returns the vert EpdGlyph* if found, nullptr otherwise.
  // Vert data must be loaded via loadVertData() first.
  const EpdGlyph* getVertGlyph(uint32_t codepoint, uint8_t style = 0) const;

  // Returns the bitmap data for a vert glyph (must be from getVertGlyph).
  const uint8_t* getVertBitmap(const EpdGlyph* vertGlyph, uint8_t style = 0) const;

  // Returns true if this font has vert data for any style.
  bool hasVertData() const;

  // Load vert glyph data from SD card for the given style.
  // Called during prewarm when vertical mode is active.
  bool loadVertData(uint8_t style);

  // Returns true if the glyph pointer points into the overflow buffer.
  bool isOverflowGlyph(const EpdGlyph* glyph) const;

  // Returns the bitmap for an on-demand-loaded (overflow) glyph.
  const uint8_t* getOverflowBitmap(const EpdGlyph* glyph) const;

  // Extract SdCardFont* from an opaque glyphMissCtx pointer.
  // Used by GfxRenderer::getGlyphBitmap() to recover the SdCardFont from EpdFontData::glyphMissCtx.
  static SdCardFont* fromMissCtx(void* ctx);

  struct Stats {
    uint32_t prewarmTotalMs = 0;
    uint32_t sdReadTimeMs = 0;
    uint32_t seekCount = 0;
    uint32_t uniqueGlyphs = 0;
    uint32_t bitmapBytes = 0;
  };
  void logStats(const char* label = "SDCF");
  void resetStats();
  const Stats& getStats() const { return stats_; }

  // Content hash of the file header + style TOC entries (computed during load).
  // Used to generate deterministic font IDs for section cache invalidation.
  uint32_t contentHash() const { return contentHash_; }

  // Original SD-card path. Used only for narrowly scoped rendering workarounds
  // where a font family's bitmap design differs from its metrics.
  const char* getFilePath() const { return filePath_; }

 private:
  // Per-style metadata (parsed from file header/TOC)
  struct CpFontHeader {
    uint32_t intervalCount = 0;
    uint32_t glyphCount = 0;
    uint8_t advanceY = 0;
    int16_t ascender = 0;
    int16_t descender = 0;
    bool is2Bit = false;
    uint16_t kernLeftEntryCount = 0;
    uint16_t kernRightEntryCount = 0;
    uint8_t kernLeftClassCount = 0;
    uint8_t kernRightClassCount = 0;
    uint8_t ligaturePairCount = 0;
  };

  // All per-style data: file offsets, intervals, kern/lig, prewarm cache, EpdFont
  struct PerStyle {
    CpFontHeader header{};

    // File layout offsets for this style's data sections
    uint32_t intervalsFileOffset = 0;
    uint32_t glyphsFileOffset = 0;
    uint32_t kernLeftFileOffset = 0;
    uint32_t kernRightFileOffset = 0;
    uint32_t kernMatrixFileOffset = 0;
    uint32_t ligatureFileOffset = 0;
    uint32_t bitmapFileOffset = 0;

    // Full intervals loaded from file (kept in RAM for codepoint lookup)
    EpdUnicodeInterval* fullIntervals = nullptr;

    // Persistent kern/ligature data (lazy-loaded on first prewarm)
    EpdKernClassEntry* kernLeftClasses = nullptr;
    EpdKernClassEntry* kernRightClasses = nullptr;
    int8_t* kernMatrix = nullptr;
    EpdLigaturePair* ligaturePairs = nullptr;
    int8_t* kernRowCache = nullptr;
    uint8_t kernCachedRows[KERN_ROW_CACHE_ROWS] = {};
    uint8_t kernRowCacheNext = 0;
    bool kernRowCacheEnabled = false;
    bool kernLigLoaded = false;

    // Stub EpdFontData returned when not prewarmed
    EpdFontData stubData{};

    // Mini EpdFontData built during prewarm
    EpdFontData miniData{};
    EpdUnicodeInterval* miniIntervals = nullptr;
    EpdGlyph* miniGlyphs = nullptr;
    uint8_t* miniBitmap = nullptr;
    // A fragmented heap may not provide one contiguous bitmap allocation for
    // a text-heavy page.  Keep the same prewarmed glyph set in small chunks.
    // Chunked fallback allocates one bitmap per glyph, avoiding a large
    // contiguous request when a retained EPUB page leaves little free heap.
    static constexpr uint8_t MAX_MINI_BITMAP_CHUNKS = 64;
    uint8_t* miniBitmapChunks[MAX_MINI_BITMAP_CHUNKS] = {};
    uint8_t miniBitmapChunkCount = 0;
    bool miniBitmapIsChunked = false;
    uint32_t miniIntervalCount = 0;
    uint32_t miniGlyphCount = 0;

    // Vertical glyph substitution data (from .cpfont v5 vert section)
    uint32_t vertSectionOffset = 0;  // File offset to vert section (0 = no vert data)
    uint16_t vertCount = 0;
    uint32_t* vertCodepoints = nullptr;
    EpdGlyph* vertGlyphs = nullptr;
    uint8_t* vertBitmap = nullptr;
    bool vertLoaded = false;

    // The EpdFont whose data pointer we manage
    EpdFont epdFont{&stubData};

    bool present = false;
  };

  PerStyle styles_[MAX_STYLES] = {};
  uint8_t styleCount_ = 0;

  char filePath_[128] = {};

  // Overflow context: glyphMissHandler needs to know which style it's serving
  struct OverflowContext {
    SdCardFont* self;
    uint8_t styleIdx;
  };
  OverflowContext overflowCtx_[MAX_STYLES] = {};

  // Shared on-demand overflow buffer (ring buffer of glyphs loaded via glyphMissHandler)
  static constexpr uint32_t OVERFLOW_CAPACITY = 8;
  struct OverflowEntry {
    EpdGlyph glyph;
    uint8_t* bitmap = nullptr;
    uint32_t codepoint = 0;
    uint8_t styleIdx = 0;
  };
  OverflowEntry overflow_[OVERFLOW_CAPACITY] = {};
  uint32_t overflowCount_ = 0;
  uint32_t overflowNext_ = 0;

  // Compact advance-only table for layout measurement (per-style).
  // Built by buildAdvanceTable(), queried by getAdvance().
  struct AdvanceEntry {
    uint32_t codepoint;
    uint16_t advanceX;  // 12.4 fixed-point
  };
  AdvanceEntry* advanceTable_[MAX_STYLES] = {};
  uint32_t advanceTableSize_[MAX_STYLES] = {};
  void clearAdvanceTables();

  Stats stats_;
  uint32_t contentHash_ = 0;
  bool loaded_ = false;

  // Per-style helpers
  void freeStyleMiniData(PerStyle& s);
  void freeStyleVertData(PerStyle& s);
  void freeStyleAll(PerStyle& s);
  void freeStyleKernLigatureData(PerStyle& s);
  bool loadStyleKernLigatureData(PerStyle& s);
  void applyKernLigaturePointers(PerStyle& s, EpdFontData& data) const;
  void applyGlyphMissCallback(uint8_t styleIdx);
  static int8_t lookupKernRow(void* ctx, uint8_t leftClass, uint8_t rightClass);
  static const uint8_t* lookupChunkedBitmap(void* ctx, const EpdGlyph* glyph);
  int32_t findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const;
  int prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly);

  // Global helpers
  void freeAll();
  void clearOverflow();
  static void computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset);

  // Static callback for EpdFontData::glyphMissHandler (per-style via OverflowContext)
  static const EpdGlyph* onGlyphMiss(void* ctx, uint32_t codepoint);
};
