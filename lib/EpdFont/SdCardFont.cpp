#include "SdCardFont.h"

#include <HalStorage.h>
#include <Issue18Diagnostics.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "EpdFontFamily.h"

static_assert(sizeof(EpdGlyph) == 16, "EpdGlyph must be 16 bytes to match .cpfont file layout");
static_assert(sizeof(EpdUnicodeInterval) == 12, "EpdUnicodeInterval must be 12 bytes to match .cpfont file layout");
static_assert(sizeof(EpdKernClassEntry) == 3, "EpdKernClassEntry must be 3 bytes to match .cpfont file layout");
static_assert(sizeof(EpdLigaturePair) == 8, "EpdLigaturePair must be 8 bytes to match .cpfont file layout");

// FNV-1a hash for content-based font ID generation
static constexpr uint32_t FNV_OFFSET = 2166136261u;
static constexpr uint32_t FNV_PRIME = 16777619u;

static uint32_t fnv1a(const uint8_t* data, size_t len, uint32_t hash = FNV_OFFSET) {
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

// .cpfont magic bytes
static constexpr char CPFONT_MAGIC[8] = {'C', 'P', 'F', 'O', 'N', 'T', '\0', '\0'};
static constexpr uint16_t CPFONT_VERSION_MIN = 4;
static constexpr uint16_t CPFONT_VERSION_MAX = 5;
static constexpr uint32_t HEADER_SIZE = 32;
static constexpr uint32_t STYLE_TOC_ENTRY_SIZE = 32;
// Vertical substitution is optional punctuation shaping.  Preserve enough
// contiguous heap for the page renderer and font fallback data before loading it.
static constexpr size_t MIN_FREE_HEAP_FOR_VERT_DATA = 32 * 1024;
// Kerning is an optional typography enhancement.  Keep a small allocator
// margin so a dense matrix never consumes the last usable contiguous block.
static constexpr size_t KERN_ALLOCATION_MARGIN = 1024;

#if defined(SD_FONT_ARENA_REUSE) && SD_FONT_ARENA_REUSE
// Retain a page arena only while enough general heap remains for the reader.
// A bitmap that stays under 75% utilization for three different rebuilds is
// treated as an outlier and released before the next rebuild.
static constexpr size_t MINI_RETAIN_MIN_FREE_HEAP = 40 * 1024;
static constexpr uint8_t MINI_UNDERUSE_RUNS_BEFORE_FREE = 3;

template <typename T>
static bool ensureArrayCapacity(T*& buffer, uint32_t& capacity, uint32_t needed) {
  if (buffer && capacity >= needed) return true;
  delete[] buffer;
  buffer = new (std::nothrow) T[needed > 0 ? needed : 1];
  capacity = buffer ? needed : 0;
  return buffer != nullptr;
}
#endif

#if defined(SD_FONT_BATCH_READS) && SD_FONT_BATCH_READS
static constexpr uint32_t BATCH_MAX_GLYPHS = 8;
static constexpr uint32_t BATCH_MAX_BITMAP_BYTES = 2048;
#endif

#if defined(SD_FONT_COALESCE_READS) && SD_FONT_COALESCE_READS
static constexpr uint32_t COALESCE_MAX_GAP_BYTES = 256;
static constexpr uint32_t COALESCE_MAX_EXTRA_BYTES_PER_STYLE = 16 * 1024;
static constexpr size_t COALESCE_SCRATCH_BYTES = 64;

static bool readAndDiscard(FsFile& file, uint32_t bytes) {
  uint8_t scratch[COALESCE_SCRATCH_BYTES];
  while (bytes > 0) {
    const size_t chunk = std::min<size_t>(bytes, sizeof(scratch));
    if (file.read(scratch, chunk) != static_cast<int>(chunk)) return false;
    bytes -= static_cast<uint32_t>(chunk);
  }
  return true;
}
#endif

// Helper to read little-endian values from byte buffer
static inline uint16_t readU16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static inline int16_t readI16(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }
static inline uint32_t readU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

SdCardFont::~SdCardFont() { freeAll(); }

// --- Per-style free/cleanup ---

void SdCardFont::freeStyleMiniData(PerStyle& s) {
  delete[] s.miniIntervals;
  s.miniIntervals = nullptr;
  delete[] s.miniGlyphs;
  s.miniGlyphs = nullptr;
  delete[] s.miniBitmap;
  s.miniBitmap = nullptr;
  s.miniIntervalCapacity = 0;
  s.miniGlyphCapacity = 0;
  s.miniBitmapCapacity = 0;
  s.miniBitmapUsed = 0;
  s.miniUnderuseRuns = 0;
  s.miniMetadataOnly = false;
  s.miniHysteresisPending = false;
  for (uint8_t i = 0; i < PerStyle::MAX_MINI_BITMAP_CHUNKS; i++) {
    free(s.miniBitmapChunks[i]);
    s.miniBitmapChunks[i] = nullptr;
  }
  s.miniBitmapChunkCount = 0;
  s.miniBitmapIsChunked = false;
  s.miniIntervalCount = 0;
  s.miniGlyphCount = 0;
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.epdFont.data = &s.stubData;
}

void SdCardFont::resetStyleMiniData(PerStyle& s) {
#if defined(SD_FONT_ARENA_REUSE) && SD_FONT_ARENA_REUSE
  if (ESP.getFreeHeap() >= MINI_RETAIN_MIN_FREE_HEAP) return;
#if defined(RENDER_PROFILE)
  stats_.retainedReleaseCount++;
#endif
#endif
  freeStyleMiniData(s);
}

void SdCardFont::freeStyleVertData(PerStyle& s) {
  delete[] s.vertCodepoints;
  s.vertCodepoints = nullptr;
  delete[] s.vertGlyphs;
  s.vertGlyphs = nullptr;
  delete[] s.vertBitmap;
  s.vertBitmap = nullptr;
  s.vertCount = 0;
  s.vertLoaded = false;
}

void SdCardFont::freeStyleKernLigatureData(PerStyle& s) {
  delete[] s.kernLeftClasses;
  s.kernLeftClasses = nullptr;
  delete[] s.kernRightClasses;
  s.kernRightClasses = nullptr;
  delete[] s.kernMatrix;
  s.kernMatrix = nullptr;
  delete[] s.ligaturePairs;
  s.ligaturePairs = nullptr;
  delete[] s.kernRowCache;
  s.kernRowCache = nullptr;
  memset(s.kernCachedRows, 0, sizeof(s.kernCachedRows));
  s.kernRowCacheNext = 0;
  s.kernRowCacheEnabled = false;
  s.kernLigLoaded = false;
}

void SdCardFont::freeStyleAll(PerStyle& s) {
  freeStyleMiniData(s);
  freeStyleVertData(s);
  delete[] s.fullIntervals;
  s.fullIntervals = nullptr;
  freeStyleKernLigatureData(s);
  s.present = false;
}

// --- Global free/cleanup ---

void SdCardFont::freeAll() {
  clearOverflow();
  clearAdvanceTables();
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    freeStyleAll(styles_[i]);
  }
  styleCount_ = 0;
  contentHash_ = 0;
  loaded_ = false;
}

void SdCardFont::releaseResidentCaches() {
  clearOverflow();
  clearAdvanceTables();
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    freeStyleMiniData(styles_[i]);
    freeStyleKernLigatureData(styles_[i]);
    applyGlyphMissCallback(i);
  }
}

void SdCardFont::releaseVerticalGlyphs() {
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    freeStyleVertData(styles_[i]);
  }
}

void SdCardFont::clearOverflow() {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    delete[] overflow_[i].bitmap;
    overflow_[i].bitmap = nullptr;
    overflow_[i].codepoint = 0;
  }
  overflowCount_ = 0;
  overflowNext_ = 0;
}

// --- Per-style kern/ligature ---

void SdCardFont::applyKernLigaturePointers(PerStyle& s, EpdFontData& data) const {
  data.kernLeftClasses = s.kernLeftClasses;
  data.kernRightClasses = s.kernRightClasses;
  data.kernMatrix = s.kernMatrix;
  data.kernLeftEntryCount = s.header.kernLeftEntryCount;
  data.kernRightEntryCount = s.header.kernRightEntryCount;
  data.kernLeftClassCount = s.header.kernLeftClassCount;
  data.kernRightClassCount = s.header.kernRightClassCount;
  data.ligaturePairs = s.ligaturePairs;
  data.ligaturePairCount = s.header.ligaturePairCount;
}

bool SdCardFont::loadStyleKernLigatureData(PerStyle& s) {
  if (s.kernLigLoaded) return true;
  bool hasKern = s.header.kernLeftEntryCount > 0;
  bool hasLig = s.header.ligaturePairCount > 0;
  if (!hasKern && !hasLig) {
    s.kernLigLoaded = true;
    return true;
  }

  FsFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for kern/lig: %s", filePath_);
    return false;
  }

  if (hasKern) {
    // Keep the allocation context in the diagnostic report.  In particular,
    // the matrix needs one contiguous block, so total free heap alone cannot
    // tell us whether this is pressure or fragmentation.
    const uint32_t freeBeforeKern = ESP.getFreeHeap();
    const uint32_t maxAllocBeforeKern = ESP.getMaxAllocHeap();
    const uint32_t matrixSize = static_cast<uint32_t>(s.header.kernLeftClassCount) * s.header.kernRightClassCount;
    // A full kerning matrix competes with the current page's glyph bitmap
    // cache.  Two active SD font bases (body and heading/ruby) can otherwise
    // consume enough contiguous heap that a text-heavy page falls back to
    // individual SD reads.  The bounded row cache keeps the same kerning data
    // available on demand without retaining one full matrix per style.
    const bool useRowCache = true;
    s.kernLeftClasses = new (std::nothrow) EpdKernClassEntry[s.header.kernLeftEntryCount];
    s.kernRightClasses = new (std::nothrow) EpdKernClassEntry[s.header.kernRightEntryCount];
    if (useRowCache) {
      const size_t cacheSize = static_cast<size_t>(KERN_ROW_CACHE_ROWS) * s.header.kernRightClassCount;
      s.kernRowCache = new (std::nothrow) int8_t[cacheSize];
      s.kernRowCacheEnabled = s.kernRowCache != nullptr;
      if (s.kernRowCacheEnabled) {
        LOG_INF("SDCF", "Using kern row cache (%u rows x %u bytes; matrix=%u maxAlloc=%u)", KERN_ROW_CACHE_ROWS,
                s.header.kernRightClassCount, matrixSize, maxAllocBeforeKern);
      }
    } else {
      s.kernMatrix = new (std::nothrow) int8_t[matrixSize];
    }

    if (!s.kernLeftClasses || !s.kernRightClasses || (!s.kernMatrix && !s.kernRowCacheEnabled)) {
      LOG_INF("SDCF",
              "Disabling kern after allocation failure (%u+%u+%u bytes; rowCache=%d; before free=%u maxAlloc=%u; "
              "after free=%u maxAlloc=%u; left=%d right=%d matrix=%d)",
              s.header.kernLeftEntryCount * 3u, s.header.kernRightEntryCount * 3u, matrixSize, freeBeforeKern,
              s.kernRowCacheEnabled, maxAllocBeforeKern, ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
              s.kernLeftClasses != nullptr, s.kernRightClasses != nullptr, s.kernMatrix != nullptr);
      freeStyleKernLigatureData(s);
      hasKern = false;
    }

    if (hasKern && !file.seekSet(s.kernLeftFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to kern data");
      freeStyleKernLigatureData(s);
      file.close();
      return false;
    }
    size_t leftSz = s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
    size_t rightSz = s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
    if (hasKern && (file.read(reinterpret_cast<uint8_t*>(s.kernLeftClasses), leftSz) != static_cast<int>(leftSz) ||
                    file.read(reinterpret_cast<uint8_t*>(s.kernRightClasses), rightSz) != static_cast<int>(rightSz) ||
                    (s.kernMatrix && file.read(reinterpret_cast<uint8_t*>(s.kernMatrix), matrixSize) !=
                                         static_cast<int>(matrixSize)))) {
      LOG_ERR("SDCF", "Failed to read kern data");
      freeStyleKernLigatureData(s);
      file.close();
      return false;
    }
  }

  if (hasLig) {
    s.ligaturePairs = new (std::nothrow) EpdLigaturePair[s.header.ligaturePairCount];
    if (!s.ligaturePairs) {
      LOG_ERR("SDCF", "Failed to allocate ligature pairs");
      freeStyleKernLigatureData(s);
      file.close();
      return false;
    }
    if (!file.seekSet(s.ligatureFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to ligature data");
      freeStyleKernLigatureData(s);
      file.close();
      return false;
    }
    size_t sz = s.header.ligaturePairCount * sizeof(EpdLigaturePair);
    if (file.read(reinterpret_cast<uint8_t*>(s.ligaturePairs), sz) != static_cast<int>(sz)) {
      LOG_ERR("SDCF", "Failed to read ligature pairs");
      freeStyleKernLigatureData(s);
      file.close();
      return false;
    }
  }

  file.close();
  s.kernLigLoaded = true;

  // Update stubData so kern/ligature is available even before prewarm
  applyKernLigaturePointers(s, s.stubData);

  LOG_DBG("SDCF", "Kern/lig loaded: kernL=%u, kernR=%u, ligs=%u", s.header.kernLeftEntryCount,
          s.header.kernRightEntryCount, s.header.ligaturePairCount);
  return true;
}

// --- Glyph miss callback ---

void SdCardFont::applyGlyphMissCallback(uint8_t styleIdx) {
  overflowCtx_[styleIdx].self = this;
  overflowCtx_[styleIdx].styleIdx = styleIdx;

  auto& s = styles_[styleIdx];
  s.stubData.glyphMissHandler = &SdCardFont::onGlyphMiss;
  s.stubData.glyphMissCtx = &overflowCtx_[styleIdx];
  s.stubData.kernLookupHandler = &SdCardFont::lookupKernRow;
  s.stubData.kernLookupCtx = &overflowCtx_[styleIdx];
}

int8_t SdCardFont::lookupKernRow(void* ctx, const uint8_t leftClass, const uint8_t rightClass) {
  auto* overflowCtx = static_cast<OverflowContext*>(ctx);
  if (!overflowCtx || !overflowCtx->self || leftClass == 0 || rightClass == 0) return 0;

  auto& s = overflowCtx->self->styles_[overflowCtx->styleIdx];
  const uint8_t rowWidth = s.header.kernRightClassCount;
  if (!s.kernRowCacheEnabled || !s.kernRowCache || rightClass > rowWidth) return 0;

  for (uint8_t slot = 0; slot < KERN_ROW_CACHE_ROWS; slot++) {
    if (s.kernCachedRows[slot] == leftClass) return s.kernRowCache[slot * rowWidth + rightClass - 1];
  }

  const uint8_t slot = s.kernRowCacheNext++ % KERN_ROW_CACHE_ROWS;
  FsFile file;
  if (!Storage.openFileForRead("SDCF", overflowCtx->self->filePath_, file)) return 0;
  const uint32_t rowOffset = s.kernMatrixFileOffset + static_cast<uint32_t>(leftClass - 1) * rowWidth;
  const bool ok = file.seekSet(rowOffset) &&
                  file.read(reinterpret_cast<uint8_t*>(s.kernRowCache + slot * rowWidth), rowWidth) == rowWidth;
  file.close();
  if (!ok) return 0;

  s.kernCachedRows[slot] = leftClass;
  return s.kernRowCache[slot * rowWidth + rightClass - 1];
}

// --- Compute per-style file offsets from a base data offset ---

void SdCardFont::computeStyleFileOffsets(PerStyle& s, uint32_t baseOffset) {
  s.intervalsFileOffset = baseOffset;
  s.glyphsFileOffset = s.intervalsFileOffset + s.header.intervalCount * sizeof(EpdUnicodeInterval);
  s.kernLeftFileOffset = s.glyphsFileOffset + s.header.glyphCount * sizeof(EpdGlyph);
  s.kernRightFileOffset = s.kernLeftFileOffset + s.header.kernLeftEntryCount * sizeof(EpdKernClassEntry);
  s.kernMatrixFileOffset = s.kernRightFileOffset + s.header.kernRightEntryCount * sizeof(EpdKernClassEntry);
  s.ligatureFileOffset =
      s.kernMatrixFileOffset + static_cast<uint32_t>(s.header.kernLeftClassCount) * s.header.kernRightClassCount;
  s.bitmapFileOffset = s.ligatureFileOffset + s.header.ligaturePairCount * sizeof(EpdLigaturePair);
}

// --- Load ---

bool SdCardFont::load(const char* path) {
  freeAll();
  if (strlen(path) >= sizeof(filePath_)) {
    LOG_ERR("SDCF", "Path too long (%zu bytes, max %zu)", strlen(path), sizeof(filePath_) - 1);
    return false;
  }
  strncpy(filePath_, path, sizeof(filePath_) - 1);
  filePath_[sizeof(filePath_) - 1] = '\0';

  FsFile file;
  if (!Storage.openFileForRead("SDCF", path, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont: %s", path);
    return false;
  }

  // Read and validate global header
  uint8_t headerBuf[HEADER_SIZE];
  if (file.read(headerBuf, HEADER_SIZE) != HEADER_SIZE) {
    LOG_ERR("SDCF", "Failed to read header");
    file.close();
    return false;
  }

  if (memcmp(headerBuf, CPFONT_MAGIC, 8) != 0) {
    LOG_ERR("SDCF", "Invalid magic bytes");
    file.close();
    return false;
  }

  uint16_t fileVersion = readU16(headerBuf + 8);
  if (fileVersion < CPFONT_VERSION_MIN || fileVersion > CPFONT_VERSION_MAX) {
    LOG_ERR("SDCF", "Unsupported version: %u (expected %u-%u)", fileVersion, CPFONT_VERSION_MIN, CPFONT_VERSION_MAX);
    file.close();
    return false;
  }

  // Begin content hash: accumulate global header
  uint32_t hash = fnv1a(headerBuf, HEADER_SIZE);

  bool is2Bit = (readU16(headerBuf + 10) & 1) != 0;

  uint8_t styleCount = headerBuf[12];
  if (styleCount == 0 || styleCount > MAX_STYLES) {
    LOG_ERR("SDCF", "Invalid style count: %u", styleCount);
    file.close();
    return false;
  }

  // Read style TOC
  for (uint8_t i = 0; i < styleCount; i++) {
    uint8_t tocBuf[STYLE_TOC_ENTRY_SIZE];
    if (file.read(tocBuf, STYLE_TOC_ENTRY_SIZE) != STYLE_TOC_ENTRY_SIZE) {
      LOG_ERR("SDCF", "Failed to read style TOC entry %u", i);
      file.close();
      freeAll();
      return false;
    }

    // Accumulate TOC entry into content hash
    hash = fnv1a(tocBuf, STYLE_TOC_ENTRY_SIZE, hash);

    uint8_t styleId = tocBuf[0];
    if (styleId >= MAX_STYLES) {
      LOG_ERR("SDCF", "Invalid styleId %u in TOC", styleId);
      continue;
    }

    auto& s = styles_[styleId];
    s.present = true;
    s.header.intervalCount = readU32(tocBuf + 4);
    s.header.glyphCount = readU32(tocBuf + 8);
    s.header.advanceY = tocBuf[12];
    s.header.ascender = readI16(tocBuf + 13);
    s.header.descender = readI16(tocBuf + 15);
    s.header.kernLeftEntryCount = readU16(tocBuf + 17);
    s.header.kernRightEntryCount = readU16(tocBuf + 19);
    s.header.kernLeftClassCount = tocBuf[21];
    s.header.kernRightClassCount = tocBuf[22];
    s.header.ligaturePairCount = tocBuf[23];
    s.header.is2Bit = is2Bit;

    // Sanity-check counts to reject malformed files before allocating
    static constexpr uint32_t MAX_INTERVALS = 4096;
    static constexpr uint32_t MAX_GLYPHS = 65536;
    if (s.header.intervalCount > MAX_INTERVALS || s.header.glyphCount > MAX_GLYPHS) {
      LOG_ERR("SDCF", "Style %u: unreasonable counts (intervals=%u, glyphs=%u)", styleId, s.header.intervalCount,
              s.header.glyphCount);
      s.present = false;
      continue;
    }

    uint32_t dataOffset = readU32(tocBuf + 24);
    computeStyleFileOffsets(s, dataOffset);

    // v5: vertSectionOffset stored at TOC offset 28 (was reserved in v4)
    if (fileVersion >= 5) {
      s.vertSectionOffset = readU32(tocBuf + 28);
    }
  }

  styleCount_ = styleCount;
  contentHash_ = hash;

  // Load full intervals into RAM for each present style
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    auto& s = styles_[i];
    if (!s.present) continue;

    s.fullIntervals = new (std::nothrow) EpdUnicodeInterval[s.header.intervalCount];
    if (!s.fullIntervals) {
      LOG_ERR("SDCF", "Failed to allocate %u intervals for style %u", s.header.intervalCount, i);
      file.close();
      freeAll();
      return false;
    }

    if (!file.seekSet(s.intervalsFileOffset)) {
      LOG_ERR("SDCF", "Failed to seek to intervals for style %u", i);
      file.close();
      freeAll();
      return false;
    }
    size_t intervalsBytes = s.header.intervalCount * sizeof(EpdUnicodeInterval);
    if (file.read(reinterpret_cast<uint8_t*>(s.fullIntervals), intervalsBytes) != static_cast<int>(intervalsBytes)) {
      LOG_ERR("SDCF", "Failed to read intervals for style %u", i);
      file.close();
      freeAll();
      return false;
    }

    // Initialize stub data
    memset(&s.stubData, 0, sizeof(s.stubData));
    s.stubData.advanceY = s.header.advanceY;
    s.stubData.ascender = s.header.ascender;
    s.stubData.descender = s.header.descender;
    s.stubData.is2Bit = s.header.is2Bit;

    s.epdFont.data = &s.stubData;
    applyGlyphMissCallback(i);
  }

  file.close();
  loaded_ = true;

  LOG_DBG("SDCF", "Loaded: %s (v%u, %u styles)", path, fileVersion, styleCount_);
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    const auto& h = styles_[i].header;
    LOG_DBG("SDCF", "  style[%u]: %u intervals, %u glyphs, advY=%u, asc=%d, desc=%d, kernL=%u, kernR=%u, ligs=%u", i,
            h.intervalCount, h.glyphCount, h.advanceY, h.ascender, h.descender, h.kernLeftEntryCount,
            h.kernRightEntryCount, h.ligaturePairCount);
  }
  return true;
}

// --- Codepoint lookup ---

int32_t SdCardFont::findGlobalGlyphIndex(const PerStyle& s, uint32_t codepoint) const {
  int left = 0;
  int right = static_cast<int>(s.header.intervalCount) - 1;
  while (left <= right) {
    int mid = left + (right - left) / 2;
    const auto& interval = s.fullIntervals[mid];
    if (codepoint < interval.first) {
      right = mid - 1;
    } else if (codepoint > interval.last) {
      left = mid + 1;
    } else {
      return static_cast<int32_t>(interval.offset + (codepoint - interval.first));
    }
  }
  return -1;
}

// --- Prewarm ---

int SdCardFont::prewarm(const char* utf8Text, uint8_t styleMask, bool metadataOnly) {
  if (!loaded_) return -1;
  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0) return 0;

  unsigned long startMs = millis();

  // Step 1: Extract unique codepoints from UTF-8 text (shared across all styles).
  // Dedup uses O(n^2) linear scan — worst case is MAX_PAGE_GLYPHS (512) unique codepoints
  // = ~131K comparisons, but in practice pages contain far fewer unique codepoints so the
  // actual cost is much lower. This is dwarfed by SD I/O that follows. Alternatives (hash
  // set, bitmap) exceed the 256-byte stack limit or add template bloat.
  // Heap-allocated: MAX_PAGE_GLYPHS * 4 = 2048 bytes, too large for stack (limit < 256 bytes)
  std::unique_ptr<uint32_t[]> codepoints(new (std::nothrow) uint32_t[MAX_PAGE_GLYPHS]);
  if (!codepoints) {
    LOG_ERR("SDCF", "Failed to allocate codepoint buffer (%u bytes)", MAX_PAGE_GLYPHS * 4);
    return -1;
  }
  uint32_t cpCount = 0;

  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8Text);
  while (*p && cpCount < MAX_PAGE_GLYPHS) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;

    bool found = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == cp) {
        found = true;
        break;
      }
    }
    if (!found) {
      codepoints[cpCount++] = cp;
    }
  }

  // Always include the replacement character
  {
    bool hasReplacement = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == REPLACEMENT_GLYPH) {
        hasReplacement = true;
        break;
      }
    }
    if (!hasReplacement && cpCount < MAX_PAGE_GLYPHS) {
      codepoints[cpCount++] = REPLACEMENT_GLYPH;
    }
  }

  // Add ligature output codepoints from all styles being prewarmed.
  // Skip during metadata-only prewarm (layout measurement) to avoid loading
  // kern/lig data for all styles upfront (~22KB per style). Kern/lig is
  // loaded per-style in prewarmStyle() during the full render prewarm instead.
  if (!metadataOnly) {
    for (uint8_t si = 0; si < MAX_STYLES; si++) {
      if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
      auto& s = styles_[si];

      loadStyleKernLigatureData(s);
      if (s.ligaturePairs && s.header.ligaturePairCount > 0) {
        for (uint8_t li = 0; li < s.header.ligaturePairCount && cpCount < MAX_PAGE_GLYPHS; li++) {
          uint32_t leftCp = s.ligaturePairs[li].pair >> 16;
          uint32_t rightCp = s.ligaturePairs[li].pair & 0xFFFF;
          uint32_t outCp = s.ligaturePairs[li].ligatureCp;

          bool hasLeft = false, hasRight = false;
          for (uint32_t i = 0; i < cpCount; i++) {
            if (codepoints[i] == leftCp) hasLeft = true;
            if (codepoints[i] == rightCp) hasRight = true;
            if (hasLeft && hasRight) break;
          }
          if (!hasLeft || !hasRight) continue;

          bool hasOut = false;
          for (uint32_t i = 0; i < cpCount; i++) {
            if (codepoints[i] == outCp) {
              hasOut = true;
              break;
            }
          }
          if (!hasOut) {
            codepoints[cpCount++] = outCp;
          }
        }
      }
    }
  }

  // Sort codepoints for ordered interval building
  std::sort(codepoints.get(), codepoints.get() + cpCount);

  // Prewarm each requested style
  int totalMissed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    totalMissed += prewarmStyle(si, codepoints.get(), cpCount, metadataOnly);
  }

  stats_.prewarmTotalMs = millis() - startMs;
  return totalMissed;
}

int SdCardFont::prewarmStyle(uint8_t styleIdx, const uint32_t* codepoints, uint32_t cpCount, bool metadataOnly) {
  auto& s = styles_[styleIdx];

#if defined(SD_FONT_REUSE_DIAGNOSTICS) && SD_FONT_REUSE_DIAGNOSTICS
  uint32_t previousGlyphCount = 0;
  uint32_t previousBitmapBytes = 0;
  uint32_t reusableGlyphCount = 0;
  uint32_t reusableBitmapBytes = 0;
  if (!metadataOnly && s.miniGlyphs && s.miniIntervals) {
    previousGlyphCount = s.miniGlyphCount;
    for (uint32_t i = 0; i < s.miniGlyphCount; i++) previousBitmapBytes += s.miniGlyphs[i].dataLength;

    for (uint32_t i = 0; i < cpCount; i++) {
      const uint32_t cp = codepoints[i];
      const EpdUnicodeInterval* begin = s.miniIntervals;
      const EpdUnicodeInterval* end = begin + s.miniIntervalCount;
      const auto it = std::upper_bound(
          begin, end, cp, [](uint32_t value, const EpdUnicodeInterval& interval) { return value < interval.first; });
      if (it == begin) continue;
      const EpdUnicodeInterval& interval = *(it - 1);
      if (cp > interval.last) continue;
      const EpdGlyph& previousGlyph = s.miniGlyphs[interval.offset + (cp - interval.first)];
      reusableGlyphCount++;
      reusableBitmapBytes += previousGlyph.dataLength;
    }
  }
#endif

#if defined(SD_FONT_ARENA_REUSE) && SD_FONT_ARENA_REUSE
  // A retained page (most notably a future idle-prewarm target) can satisfy a
  // later request without touching the SD card. Metadata-only data cannot
  // serve a full bitmap request.
  if (s.miniGlyphCount > 0 && !(s.miniMetadataOnly && !metadataOnly)) {
    bool covered = true;
    int missedInMini = 0;
    for (uint32_t i = 0; i < cpCount && covered; i++) {
      const uint32_t cp = codepoints[i];
      bool inMini = false;
      for (uint32_t intervalIndex = 0; intervalIndex < s.miniIntervalCount; intervalIndex++) {
        const EpdUnicodeInterval& interval = s.miniIntervals[intervalIndex];
        if (cp < interval.first) break;
        if (cp <= interval.last) {
          inMini = true;
          break;
        }
      }
      if (inMini) continue;
      if (findGlobalGlyphIndex(s, cp) < 0) {
        missedInMini++;
      } else {
        covered = false;
      }
    }
    if (covered) {
#if defined(RENDER_PROFILE)
      stats_.residentHitCount++;
#endif
      return missedInMini;
    }
  }

  // Consider trimming an outlier only after the retained data has had a chance
  // to satisfy the next request. This avoids discarding a freshly prepared
  // page before it can be drawn.
  if (s.miniHysteresisPending && s.miniBitmapCapacity > 0 && s.miniBitmapUsed > 0) {
    s.miniHysteresisPending = false;
    if (s.miniBitmapUsed < s.miniBitmapCapacity - s.miniBitmapCapacity / 4) {
      if (++s.miniUnderuseRuns >= MINI_UNDERUSE_RUNS_BEFORE_FREE) {
        freeStyleMiniData(s);
#if defined(RENDER_PROFILE)
        stats_.retainedReleaseCount++;
#endif
      }
    } else {
      s.miniUnderuseRuns = 0;
    }
  }

  // A chunked cache has no reusable contiguous arena. Release it on a miss;
  // the existing chunked allocation path remains the low-memory fallback.
  if (s.miniBitmapIsChunked) freeStyleMiniData(s);
#endif

  // Layout-only measurements must not replace a resident bitmap arena. Doing
  // so makes the following render fetch every page glyph through the overflow
  // path, and grayscale bands repeat those SD reads several times.
  if (metadataOnly && !s.miniMetadataOnly && s.miniGlyphCount > 0 && s.miniBitmapUsed > 0) {
    LOG_DBG("SDCF", "Prewarm: keeping bitmap arena for metadata-only request (style=%u cps=%u)", styleIdx, cpCount);
    return 0;
  }

  // Map codepoints to global glyph indices for this style
  struct CpGlyphMapping {
    uint32_t codepoint;
    int32_t globalIndex;
  };
  CpGlyphMapping* mappings = new (std::nothrow) CpGlyphMapping[cpCount];
  if (!mappings) {
    LOG_ERR("SDCF", "Failed to allocate mapping array for style %u", styleIdx);
    return static_cast<int>(cpCount);
  }

  uint32_t validCount = 0;
  for (uint32_t i = 0; i < cpCount; i++) {
    int32_t idx = findGlobalGlyphIndex(s, codepoints[i]);
    if (idx >= 0) {
      mappings[validCount].codepoint = codepoints[i];
      mappings[validCount].globalIndex = idx;
      validCount++;
    }
  }
  int missed = static_cast<int>(cpCount - validCount);

  if (validCount == 0) {
#if defined(SD_FONT_ARENA_REUSE) && SD_FONT_ARENA_REUSE
    s.miniIntervalCount = 0;
    s.miniGlyphCount = 0;
    s.miniBitmapUsed = 0;
    s.miniMetadataOnly = metadataOnly;
    s.miniHysteresisPending = false;
    memset(&s.miniData, 0, sizeof(s.miniData));
#else
    freeStyleMiniData(s);
#endif
    delete[] mappings;
    s.epdFont.data = &s.stubData;
    return missed;
  }

  // Build mini intervals from sorted codepoints
#if defined(SD_FONT_ARENA_REUSE) && SD_FONT_ARENA_REUSE
  s.miniIntervalCount = 0;
  s.miniGlyphCount = 0;
  s.epdFont.data = &s.stubData;
#else
  freeStyleMiniData(s);
#endif

  uint32_t intervalCapacity = validCount;
#if defined(SD_FONT_ARENA_REUSE) && SD_FONT_ARENA_REUSE
  if (!ensureArrayCapacity(s.miniIntervals, s.miniIntervalCapacity, intervalCapacity)) {
#else
  s.miniIntervals = new (std::nothrow) EpdUnicodeInterval[intervalCapacity];
  if (!s.miniIntervals) {
#endif
    LOG_ERR("SDCF", "Failed to allocate mini intervals for style %u", styleIdx);
    delete[] mappings;
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }

  s.miniIntervalCount = 0;
  uint32_t rangeStart = 0;
  for (uint32_t i = 1; i <= validCount; i++) {
    if (i == validCount || mappings[i].codepoint != mappings[i - 1].codepoint + 1) {
      s.miniIntervals[s.miniIntervalCount].first = mappings[rangeStart].codepoint;
      s.miniIntervals[s.miniIntervalCount].last = mappings[i - 1].codepoint;
      s.miniIntervals[s.miniIntervalCount].offset = rangeStart;
      s.miniIntervalCount++;
      rangeStart = i;
    }
  }

  // Allocate mini glyph array
  s.miniGlyphCount = validCount;
#if defined(SD_FONT_ARENA_REUSE) && SD_FONT_ARENA_REUSE
  if (!ensureArrayCapacity(s.miniGlyphs, s.miniGlyphCapacity, s.miniGlyphCount)) {
#else
  s.miniGlyphs = new (std::nothrow) EpdGlyph[s.miniGlyphCount];
  if (!s.miniGlyphs) {
#endif
    LOG_ERR("SDCF", "Failed to allocate mini glyphs for style %u", styleIdx);
    delete[] mappings;
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }

  // Build sorted read order for sequential I/O
  uint32_t* readOrder = new (std::nothrow) uint32_t[validCount];
  if (!readOrder) {
    LOG_ERR("SDCF", "Failed to allocate read order for style %u", styleIdx);
    delete[] mappings;
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }
  for (uint32_t i = 0; i < validCount; i++) readOrder[i] = i;
  std::sort(readOrder, readOrder + validCount,
            [&](uint32_t a, uint32_t b) { return mappings[a].globalIndex < mappings[b].globalIndex; });

  FsFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to reopen .cpfont for prewarm (style %u)", styleIdx);
    delete[] readOrder;
    delete[] mappings;
    freeStyleMiniData(s);
    return static_cast<int>(cpCount);
  }

  unsigned long sdStart = millis();
  uint32_t seekCount = 0;
#if defined(RENDER_PROFILE)
  uint32_t metadataSeekCount = 0;
  uint32_t bitmapSeekCount = 0;
  uint32_t metadataReadCount = 0;
  uint32_t bitmapReadCount = 0;
  uint32_t metadataBatchCount = 0;
  uint32_t bitmapBatchCount = 0;
  uint32_t metadataBatchedGlyphs = 0;
  uint32_t bitmapBatchedGlyphs = 0;
  uint32_t maxBatchBytes = 0;
  uint32_t coalescedGapCount = 0;
  uint32_t readAheadBytes = 0;
#endif
#if defined(SD_FONT_COALESCE_READS) && SD_FONT_COALESCE_READS
  uint32_t readAheadBudgetUsed = 0;
#endif

  // Read glyph metadata in file order. Small forward gaps can be cheaper to
  // consume sequentially than to seek past on SD, while the extra-read budget
  // keeps sparse CJK pages bounded.
  uint32_t lastMetadataEnd = UINT32_MAX;
  for (uint32_t i = 0; i < validCount;) {
    uint32_t mapIdx = readOrder[i];
    int32_t gIdx = mappings[mapIdx].globalIndex;

    uint32_t batchGlyphs = 1;
#if defined(SD_FONT_BATCH_READS) && SD_FONT_BATCH_READS
    while (batchGlyphs < BATCH_MAX_GLYPHS && i + batchGlyphs < validCount) {
      const uint32_t nextMapIdx = readOrder[i + batchGlyphs];
      const int32_t nextGIdx = mappings[nextMapIdx].globalIndex;
      if (nextGIdx != gIdx + static_cast<int32_t>(batchGlyphs) || nextMapIdx != mapIdx + batchGlyphs) break;
      batchGlyphs++;
    }
#endif

    uint32_t fileOff = s.glyphsFileOffset + static_cast<uint32_t>(gIdx) * sizeof(EpdGlyph);
    if (fileOff != lastMetadataEnd) {
      bool continued = false;
#if defined(SD_FONT_COALESCE_READS) && SD_FONT_COALESCE_READS
      if (lastMetadataEnd != UINT32_MAX && fileOff > lastMetadataEnd) {
        const uint32_t gap = fileOff - lastMetadataEnd;
        if (gap <= COALESCE_MAX_GAP_BYTES && gap <= COALESCE_MAX_EXTRA_BYTES_PER_STYLE - readAheadBudgetUsed) {
          if (!readAndDiscard(file, gap)) {
            LOG_ERR("SDCF", "Prewarm: short metadata read-ahead (style %u)", styleIdx);
            file.close();
            delete[] readOrder;
            delete[] mappings;
            freeStyleMiniData(s);
            return static_cast<int>(cpCount);
          }
          readAheadBudgetUsed += gap;
          continued = true;
#if defined(RENDER_PROFILE)
          coalescedGapCount++;
          readAheadBytes += gap;
#endif
        }
      }
#endif
      if (!continued) {
        file.seekSet(fileOff);
        seekCount++;
#if defined(RENDER_PROFILE)
        metadataSeekCount++;
#endif
      }
    }
    const uint32_t batchBytes = batchGlyphs * sizeof(EpdGlyph);
    if (file.read(reinterpret_cast<uint8_t*>(&s.miniGlyphs[mapIdx]), batchBytes) != static_cast<int>(batchBytes)) {
      LOG_ERR("SDCF", "Prewarm: short glyph read (style %u, glyph %d)", styleIdx, gIdx);
      file.close();
      delete[] readOrder;
      delete[] mappings;
      freeStyleMiniData(s);
      return static_cast<int>(cpCount);
    }
#if defined(RENDER_PROFILE)
    metadataReadCount++;
    if (batchGlyphs > 1) {
      metadataBatchCount++;
      metadataBatchedGlyphs += batchGlyphs;
      maxBatchBytes = std::max(maxBatchBytes, batchBytes);
    }
#endif
    lastMetadataEnd = fileOff + batchBytes;
    i += batchGlyphs;
  }

  uint32_t totalBitmapSize = 0;

  if (!metadataOnly) {
    // Compute total bitmap size
    for (uint32_t i = 0; i < validCount; i++) {
      totalBitmapSize += s.miniGlyphs[i].dataLength;
    }

    const uint32_t bitmapAllocationSize = totalBitmapSize > 0 ? totalBitmapSize : 1;
#if defined(SD_FONT_ARENA_REUSE) && SD_FONT_ARENA_REUSE
    const bool bitmapArenaFits = s.miniBitmap && s.miniBitmapCapacity >= bitmapAllocationSize;
    if (!bitmapArenaFits) {
      // Metadata is already resident in miniGlyphs. Drop the old bitmap before
      // growing it so the allocator can coalesce that region, then continue
      // directly to bitmap I/O without a second metadata pass.
      delete[] s.miniBitmap;
      s.miniBitmap = nullptr;
      s.miniBitmapCapacity = 0;
      s.miniBitmap = new (std::nothrow) uint8_t[bitmapAllocationSize];
      if (s.miniBitmap) s.miniBitmapCapacity = bitmapAllocationSize;
#if defined(RENDER_PROFILE)
      stats_.bitmapArenaGrowCount++;
#endif
    } else {
#if defined(RENDER_PROFILE)
      stats_.bitmapArenaReuseCount++;
#endif
    }
#else
    s.miniBitmap = new (std::nothrow) uint8_t[bitmapAllocationSize];
#endif
    if (!s.miniBitmap) {
      // Preserve the batch-read path when the heap has no sufficiently large
      // contiguous region.  Each glyph resides in one small allocation.
      s.miniBitmapIsChunked = true;
      LOG_INF("SDCF", "Using chunked mini bitmap (%u bytes) for style %u", totalBitmapSize, styleIdx);
    } else {
#if !defined(SD_FONT_ARENA_REUSE) || !SD_FONT_ARENA_REUSE
      s.miniBitmapCapacity = bitmapAllocationSize;
#endif
    }

    // Read bitmap data sorted by file offset
    std::sort(readOrder, readOrder + validCount,
              [&](uint32_t a, uint32_t b) { return s.miniGlyphs[a].dataOffset < s.miniGlyphs[b].dataOffset; });

    uint32_t miniBitmapOffset = 0;
    uint8_t chunkIndex = 0;
    uint32_t lastBitmapEnd = UINT32_MAX;
    for (uint32_t i = 0; i < validCount;) {
      uint32_t mapIdx = readOrder[i];
      EpdGlyph& glyph = s.miniGlyphs[mapIdx];

      if (glyph.dataLength == 0) {
        glyph.dataOffset = s.miniBitmapIsChunked ? 0 : miniBitmapOffset;
        i++;
        continue;
      }

      uint32_t batchGlyphs = 1;
      uint32_t batchBytes = glyph.dataLength;
#if defined(SD_FONT_BATCH_READS) && SD_FONT_BATCH_READS
      if (!s.miniBitmapIsChunked) {
        uint32_t expectedNextOffset = glyph.dataOffset + glyph.dataLength;
        while (batchGlyphs < BATCH_MAX_GLYPHS && i + batchGlyphs < validCount) {
          const uint32_t nextMapIdx = readOrder[i + batchGlyphs];
          const EpdGlyph& nextGlyph = s.miniGlyphs[nextMapIdx];
          if (nextGlyph.dataLength == 0 || nextGlyph.dataOffset != expectedNextOffset ||
              batchBytes + nextGlyph.dataLength > BATCH_MAX_BITMAP_BYTES) {
            break;
          }
          batchBytes += nextGlyph.dataLength;
          expectedNextOffset += nextGlyph.dataLength;
          batchGlyphs++;
        }
      }
#endif

      if (s.miniBitmapIsChunked && s.miniBitmapChunkCount >= PerStyle::MAX_MINI_BITMAP_CHUNKS) {
        LOG_ERR("SDCF", "Failed to allocate chunked mini bitmap (%u bytes) for style %u", totalBitmapSize, styleIdx);
        file.close();
        delete[] readOrder;
        delete[] mappings;
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }
      if (s.miniBitmapIsChunked) {
        chunkIndex = s.miniBitmapChunkCount;
        // On ESP32-C3, a failed nothrow new[] can still terminate instead of
        // returning nullptr. malloc() lets this low-heap fallback fail safely.
        s.miniBitmapChunks[chunkIndex] = static_cast<uint8_t*>(malloc(glyph.dataLength));
        if (!s.miniBitmapChunks[chunkIndex]) {
          LOG_ERR("SDCF", "Failed to allocate chunked mini bitmap (%u bytes) for style %u", totalBitmapSize, styleIdx);
          file.close();
          delete[] readOrder;
          delete[] mappings;
          freeStyleMiniData(s);
          return static_cast<int>(cpCount);
        }
        s.miniBitmapChunkCount++;
      }

      uint32_t fileOff = s.bitmapFileOffset + glyph.dataOffset;
      if (fileOff != lastBitmapEnd) {
        bool continued = false;
#if defined(SD_FONT_COALESCE_READS) && SD_FONT_COALESCE_READS
        if (lastBitmapEnd != UINT32_MAX && fileOff > lastBitmapEnd) {
          const uint32_t gap = fileOff - lastBitmapEnd;
          if (gap <= COALESCE_MAX_GAP_BYTES && gap <= COALESCE_MAX_EXTRA_BYTES_PER_STYLE - readAheadBudgetUsed) {
            if (!readAndDiscard(file, gap)) {
              LOG_ERR("SDCF", "Prewarm: short bitmap read-ahead (style %u)", styleIdx);
              file.close();
              delete[] readOrder;
              delete[] mappings;
              freeStyleMiniData(s);
              return static_cast<int>(cpCount);
            }
            readAheadBudgetUsed += gap;
            continued = true;
#if defined(RENDER_PROFILE)
            coalescedGapCount++;
            readAheadBytes += gap;
#endif
          }
        }
#endif
        if (!continued) {
          file.seekSet(fileOff);
          seekCount++;
#if defined(RENDER_PROFILE)
          bitmapSeekCount++;
#endif
        }
      }
      uint8_t* const target = s.miniBitmapIsChunked ? s.miniBitmapChunks[chunkIndex] : s.miniBitmap + miniBitmapOffset;
      if (file.read(target, batchBytes) != static_cast<int>(batchBytes)) {
        LOG_ERR("SDCF", "Prewarm: short bitmap read (style %u)", styleIdx);
        file.close();
        delete[] readOrder;
        delete[] mappings;
        freeStyleMiniData(s);
        return static_cast<int>(cpCount);
      }
#if defined(RENDER_PROFILE)
      bitmapReadCount++;
      if (batchGlyphs > 1) {
        bitmapBatchCount++;
        bitmapBatchedGlyphs += batchGlyphs;
        maxBatchBytes = std::max(maxBatchBytes, batchBytes);
      }
#endif
      lastBitmapEnd = fileOff + batchBytes;

      if (s.miniBitmapIsChunked) {
        glyph.dataOffset = static_cast<uint32_t>(chunkIndex) << 16;
      } else {
        uint32_t packedOffset = miniBitmapOffset;
        for (uint32_t batchIndex = 0; batchIndex < batchGlyphs; batchIndex++) {
          EpdGlyph& batchGlyph = s.miniGlyphs[readOrder[i + batchIndex]];
          batchGlyph.dataOffset = packedOffset;
          packedOffset += batchGlyph.dataLength;
        }
        miniBitmapOffset += batchBytes;
      }
      i += batchGlyphs;
    }
  }

#if defined(SD_FONT_ARENA_REUSE) && SD_FONT_ARENA_REUSE
  s.miniBitmapUsed = metadataOnly ? 0 : totalBitmapSize;
  s.miniMetadataOnly = metadataOnly;
  s.miniHysteresisPending = !metadataOnly;
#endif

  uint32_t sdTime = millis() - sdStart;
  file.close();
  delete[] readOrder;
  delete[] mappings;

  // Lazy-load kern/ligature data (skip during metadata-only prewarm to avoid
  // ~22KB per style allocation during layout measurement — layout only needs advanceX)
  bool kernLigOk = false;
  if (!metadataOnly) {
    kernLigOk = loadStyleKernLigatureData(s);
  }

  // Populate miniData and swap
  memset(&s.miniData, 0, sizeof(s.miniData));
  s.miniData.bitmap = metadataOnly ? nullptr : s.miniBitmap;
  s.miniData.glyph = s.miniGlyphs;
  s.miniData.intervals = s.miniIntervals;
  s.miniData.intervalCount = s.miniIntervalCount;
  s.miniData.advanceY = s.header.advanceY;
  s.miniData.ascender = s.header.ascender;
  s.miniData.descender = s.header.descender;
  s.miniData.is2Bit = s.header.is2Bit;
  if (kernLigOk) {
    applyKernLigaturePointers(s, s.miniData);
  }
  s.miniData.glyphMissHandler = &SdCardFont::onGlyphMiss;
  s.miniData.glyphMissCtx = &overflowCtx_[styleIdx];
  s.miniData.bitmapLookupHandler = s.miniBitmapIsChunked ? &SdCardFont::lookupChunkedBitmap : nullptr;
  s.miniData.bitmapLookupCtx = s.miniBitmapIsChunked ? &overflowCtx_[styleIdx] : nullptr;
  s.miniData.kernLookupHandler = &SdCardFont::lookupKernRow;
  s.miniData.kernLookupCtx = &overflowCtx_[styleIdx];

  s.epdFont.data = &s.miniData;

  // Accumulate stats
  stats_.sdReadTimeMs += sdTime;
  stats_.seekCount += seekCount;
  stats_.uniqueGlyphs += validCount;
  stats_.bitmapBytes += totalBitmapSize;
#if defined(RENDER_PROFILE)
  stats_.metadataSeekCount += metadataSeekCount;
  stats_.bitmapSeekCount += bitmapSeekCount;
  stats_.metadataReadCount += metadataReadCount;
  stats_.bitmapReadCount += bitmapReadCount;
  stats_.metadataBatchCount += metadataBatchCount;
  stats_.bitmapBatchCount += bitmapBatchCount;
  stats_.metadataBatchedGlyphs += metadataBatchedGlyphs;
  stats_.bitmapBatchedGlyphs += bitmapBatchedGlyphs;
  stats_.maxBatchBytes = std::max(stats_.maxBatchBytes, maxBatchBytes);
  stats_.coalescedGapCount += coalescedGapCount;
  stats_.readAheadBytes += readAheadBytes;
#endif

#if defined(SD_FONT_REUSE_DIAGNOSTICS) && SD_FONT_REUSE_DIAGNOSTICS
  if (!metadataOnly) {
    const uint32_t reusePercent = totalBitmapSize > 0 ? reusableBitmapBytes * 100 / totalBitmapSize : 0;
    LOG_INF("SDCR", "font=%08lX style=%u cur=%u prev=%u hit=%u bytes=%u/%u pct=%u prev_bytes=%u",
            static_cast<unsigned long>(contentHash_), styleIdx, validCount, previousGlyphCount, reusableGlyphCount,
            reusableBitmapBytes, totalBitmapSize, reusePercent, previousBitmapBytes);
  }
#endif

  return missed;
}

// --- Cache management ---

void SdCardFont::resetPageCache() {
  clearOverflow();
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    resetStyleMiniData(styles_[i]);
    applyGlyphMissCallback(i);
  }
}

void SdCardFont::clearCache() {
  clearOverflow();
  clearAdvanceTables();
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    freeStyleMiniData(styles_[i]);
    applyGlyphMissCallback(i);
  }
}

void SdCardFont::freeKernLigatureData() {
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (!styles_[i].present) continue;
    freeStyleKernLigatureData(styles_[i]);
    // Clear pointers in stubData/miniData too
    styles_[i].stubData.kernLeftClasses = nullptr;
    styles_[i].stubData.kernRightClasses = nullptr;
    styles_[i].stubData.kernMatrix = nullptr;
    styles_[i].stubData.ligaturePairs = nullptr;
    styles_[i].miniData.kernLeftClasses = nullptr;
    styles_[i].miniData.kernRightClasses = nullptr;
    styles_[i].miniData.kernMatrix = nullptr;
    styles_[i].miniData.ligaturePairs = nullptr;
  }
}

// --- Vertical glyph substitution ---

const EpdGlyph* SdCardFont::getVertGlyph(uint32_t codepoint, uint8_t style) const {
  if (style >= MAX_STYLES || !styles_[style].vertLoaded || styles_[style].vertCount == 0) return nullptr;

  const auto& s = styles_[style];
  uint32_t lo = 0, hi = s.vertCount;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (s.vertCodepoints[mid] < codepoint)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < s.vertCount && s.vertCodepoints[lo] == codepoint) {
    return &s.vertGlyphs[lo];
  }
  return nullptr;
}

const uint8_t* SdCardFont::getVertBitmap(const EpdGlyph* vertGlyph, uint8_t style) const {
  if (!vertGlyph || style >= MAX_STYLES || !styles_[style].vertBitmap) return nullptr;
  return styles_[style].vertBitmap + vertGlyph->dataOffset;
}

bool SdCardFont::hasVertData() const {
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (styles_[i].present && styles_[i].vertSectionOffset != 0) return true;
  }
  return false;
}

bool SdCardFont::loadVertData(uint8_t style) {
  if (style >= MAX_STYLES || !styles_[style].present) return false;
  auto& s = styles_[style];
  if (s.vertLoaded) return true;
  if (s.vertSectionOffset == 0) return false;

  if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_VERT_DATA || ESP.getMaxAllocHeap() < MIN_FREE_HEAP_FOR_VERT_DATA) {
    LOG_DBG("SDCF", "Skipping vert data for style %u (free=%u, maxAlloc=%u, need>=%zu)", style, ESP.getFreeHeap(),
            ESP.getMaxAllocHeap(), MIN_FREE_HEAP_FOR_VERT_DATA);
    Issue18Diagnostics::logMemory("vert-load-skipped", filePath_);
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) {
    LOG_ERR("SDCF", "Failed to open .cpfont for vert: %s", filePath_);
    return false;
  }

  if (!file.seekSet(s.vertSectionOffset)) {
    LOG_ERR("SDCF", "Failed to seek to vert section for style %u", style);
    file.close();
    return false;
  }

  // Read vert count
  uint8_t countBuf[2];
  if (file.read(countBuf, 2) != 2) {
    LOG_ERR("SDCF", "Failed to read vert count");
    file.close();
    return false;
  }
  s.vertCount = readU16(countBuf);

  if (s.vertCount == 0) {
    file.close();
    s.vertLoaded = true;
    return true;
  }

  // Read codepoint + EpdGlyph entries
  // Each entry: uint32_t codepoint (4) + EpdGlyph (16) = 20 bytes
  static constexpr uint32_t VERT_ENTRY_SIZE = 4 + sizeof(EpdGlyph);  // 20 bytes
  uint32_t entriesSize = s.vertCount * VERT_ENTRY_SIZE;
  auto* entryBuf = new (std::nothrow) uint8_t[entriesSize];
  if (!entryBuf) {
    LOG_ERR("SDCF", "Failed to allocate vert entry buffer (%u bytes)", entriesSize);
    file.close();
    return false;
  }

  if (file.read(entryBuf, entriesSize) != static_cast<int>(entriesSize)) {
    LOG_ERR("SDCF", "Failed to read vert entries");
    delete[] entryBuf;
    file.close();
    return false;
  }

  s.vertCodepoints = new (std::nothrow) uint32_t[s.vertCount];
  s.vertGlyphs = new (std::nothrow) EpdGlyph[s.vertCount];
  if (!s.vertCodepoints || !s.vertGlyphs) {
    LOG_ERR("SDCF", "Failed to allocate vert glyph arrays");
    delete[] entryBuf;
    delete[] s.vertCodepoints;
    s.vertCodepoints = nullptr;
    delete[] s.vertGlyphs;
    s.vertGlyphs = nullptr;
    file.close();
    return false;
  }

  uint32_t totalVertBitmapSize = 0;
  for (uint16_t i = 0; i < s.vertCount; i++) {
    const uint8_t* p = entryBuf + i * VERT_ENTRY_SIZE;
    s.vertCodepoints[i] = readU32(p);
    memcpy(&s.vertGlyphs[i], p + 4, sizeof(EpdGlyph));
    totalVertBitmapSize += s.vertGlyphs[i].dataLength;
  }
  delete[] entryBuf;

  // Read vert bitmaps
  if (totalVertBitmapSize > 0) {
    s.vertBitmap = new (std::nothrow) uint8_t[totalVertBitmapSize];
    if (!s.vertBitmap) {
      LOG_ERR("SDCF", "Failed to allocate vert bitmap (%u bytes)", totalVertBitmapSize);
      delete[] s.vertCodepoints;
      s.vertCodepoints = nullptr;
      delete[] s.vertGlyphs;
      s.vertGlyphs = nullptr;
      file.close();
      return false;
    }
    if (file.read(s.vertBitmap, totalVertBitmapSize) != static_cast<int>(totalVertBitmapSize)) {
      LOG_ERR("SDCF", "Failed to read vert bitmaps");
      delete[] s.vertCodepoints;
      s.vertCodepoints = nullptr;
      delete[] s.vertGlyphs;
      s.vertGlyphs = nullptr;
      delete[] s.vertBitmap;
      s.vertBitmap = nullptr;
      file.close();
      return false;
    }
  }

  file.close();
  s.vertLoaded = true;
  LOG_DBG("SDCF", "Vert loaded: style=%u, count=%u, bitmaps=%u bytes", style, s.vertCount, totalVertBitmapSize);
  Issue18Diagnostics::logMemory("vert-load-success", filePath_);
  return true;
}

// --- Advance table ---

void SdCardFont::clearAdvanceTables() {
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    delete[] advanceTable_[i];
    advanceTable_[i] = nullptr;
    advanceTableSize_[i] = 0;
  }
}

bool SdCardFont::hasAdvanceTable() const {
  for (uint8_t i = 0; i < MAX_STYLES; i++) {
    if (advanceTable_[i]) return true;
  }
  return false;
}

uint16_t SdCardFont::getAdvance(uint32_t codepoint, uint8_t style) const {
  // Fall back to Regular (style 0) if requested style has no advance table
  // (e.g., Bold requested but font only has Regular)
  if (style >= MAX_STYLES || !advanceTable_[style]) {
    if (style != 0 && advanceTable_[0]) {
      style = 0;  // fallback to Regular
    } else {
      return 0;
    }
  }
  const AdvanceEntry* table = advanceTable_[style];
  const uint32_t size = advanceTableSize_[style];
  // Binary search sorted by codepoint
  uint32_t lo = 0, hi = size;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (table[mid].codepoint < codepoint) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < size && table[lo].codepoint == codepoint) {
    return table[lo].advanceX;
  }
  return 0;
}

uint16_t SdCardFont::readAdvanceOnly(const uint32_t codepoint, uint8_t style) const {
  style = resolveStyle(style);
  if (!loaded_ || style >= MAX_STYLES || !styles_[style].present) return 0;
  const auto& s = styles_[style];

  if (s.miniData.intervals && s.miniData.intervalCount > 0 && s.miniData.glyph) {
    const auto* begin = s.miniData.intervals;
    const auto* end = begin + s.miniData.intervalCount;
    const auto it = std::upper_bound(
        begin, end, codepoint,
        [](const uint32_t value, const EpdUnicodeInterval& interval) { return value < interval.first; });
    if (it != begin) {
      const auto& interval = *(it - 1);
      if (codepoint <= interval.last) {
        return s.miniData.glyph[interval.offset + (codepoint - interval.first)].advanceX;
      }
    }
  }

  for (uint32_t i = 0; i < overflowCount_; ++i) {
    if (overflow_[i].codepoint == codepoint && overflow_[i].styleIdx == style) return overflow_[i].glyph.advanceX;
  }

  const int32_t globalIndex = findGlobalGlyphIndex(s, codepoint);
  if (globalIndex < 0) return 0;
  FsFile file;
  if (!Storage.openFileForRead("SDCF", filePath_, file)) return 0;
  EpdGlyph glyph = {};
  const uint32_t offset = s.glyphsFileOffset + static_cast<uint32_t>(globalIndex) * sizeof(EpdGlyph);
  if (!file.seekSet(offset) || file.read(reinterpret_cast<uint8_t*>(&glyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
    return 0;
  }
  return glyph.advanceX;
}

uint16_t SdCardFont::getAdvanceOrLoad(const uint32_t codepoint, const uint8_t style) {
  uint16_t advance = 0;
  return tryGetAdvanceOrLoad(codepoint, style, advance) ? advance : 0;
}

const uint8_t* SdCardFont::lookupChunkedBitmap(void* ctx, const EpdGlyph* glyph) {
  auto* overflowCtx = static_cast<OverflowContext*>(ctx);
  if (!overflowCtx || !overflowCtx->self || !glyph) return nullptr;

  const auto& s = overflowCtx->self->styles_[overflowCtx->styleIdx];
  if (!s.miniBitmapIsChunked) return nullptr;

  const uint8_t chunk = static_cast<uint8_t>(glyph->dataOffset >> 16);
  const uint16_t offset = static_cast<uint16_t>(glyph->dataOffset & 0xFFFF);
  if (chunk >= s.miniBitmapChunkCount || !s.miniBitmapChunks[chunk]) return nullptr;
  return s.miniBitmapChunks[chunk] + offset;
}

bool SdCardFont::tryGetAdvanceOrLoad(const uint32_t codepoint, uint8_t style, uint16_t& advanceOut) {
  // Unlike getAdvance(), retain the distinction between an existing zero-width
  // glyph and an absent table entry.
  if (style >= MAX_STYLES || !advanceTable_[style]) {
    if (style != 0 && advanceTable_[0]) {
      style = 0;
    } else {
      return false;
    }
  }

  const AdvanceEntry* table = advanceTable_[style];
  const uint32_t size = advanceTableSize_[style];
  uint32_t lo = 0;
  uint32_t hi = size;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (table[mid].codepoint < codepoint) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < size && table[lo].codepoint == codepoint) {
    advanceOut = table[lo].advanceX;
    return true;
  }

  // The compact table intentionally has a memory bound.  A missing entry must
  // first be checked through the existing overflow path, which also supplies
  // the later render operation.
  const uint8_t resolvedStyle = resolveStyle(style);
  const EpdGlyph* glyph = onGlyphMiss(&overflowCtx_[resolvedStyle], codepoint);
  if (!glyph) return false;
  advanceOut = glyph->advanceX;
  return true;
}

int SdCardFont::buildAdvanceTable(const char* utf8Text, uint8_t styleMask) {
  if (!loaded_) return -1;

  styleMask = resolveStyleMask(styleMask);
  if (styleMask == 0) return 0;

  unsigned long startMs = millis();

  // Step 1: Extract unique codepoints (no limit).
  // First pass: count total codepoints to size the dedup buffer.
  uint32_t totalChars = 0;
  {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8Text);
    while (*p) {
      utf8NextCodepoint(&p);
      totalChars++;
    }
  }
  if (totalChars == 0) return 0;

  // Allocate buffer for unique codepoints.
  // Cap at MAX_ADVANCE_CODEPOINTS to prevent OOM on large CJK sections.
  // Codepoints beyond the cap are still renderable via onGlyphMiss(), just
  // without a cached advance value (falls back to SD read).
  static constexpr uint32_t MAX_ADVANCE_CODEPOINTS = 1024;
  const uint32_t bufSize = (totalChars < MAX_ADVANCE_CODEPOINTS) ? totalChars : MAX_ADVANCE_CODEPOINTS;
  uint32_t* codepoints = new (std::nothrow) uint32_t[bufSize];
  if (!codepoints) {
    LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate codepoint buffer (%u bytes)", bufSize * 4);
    return -1;
  }
  uint32_t cpCount = 0;

  // Second pass: collect unique codepoints via O(n²) dedup.
  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8Text);
  while (*p && cpCount < bufSize) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;

    bool found = false;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (codepoints[i] == cp) {
        found = true;
        break;
      }
    }
    if (!found) {
      codepoints[cpCount++] = cp;
    }
  }

  // Sort for ordered glyph index mapping and final table output
  std::sort(codepoints, codepoints + cpCount);

  // Step 2: Extend the per-style tables with only codepoints not already
  // cached for this section. Rebuilding the complete table for every HTML
  // paragraph causes hundreds of repeated cpfont reads in prose-heavy EPUBs.
  int totalMissed = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (!(styleMask & (1 << si)) || !styles_[si].present) continue;
    const auto& s = styles_[si];
    const uint32_t oldCount = advanceTableSize_[si];
    if (oldCount >= MAX_ADVANCE_CODEPOINTS) continue;

    const auto isAlreadyCached = [this, si](const uint32_t codepoint) {
      const AdvanceEntry* table = advanceTable_[si];
      uint32_t lo = 0;
      uint32_t hi = advanceTableSize_[si];
      while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (table[mid].codepoint < codepoint) {
          lo = mid + 1;
        } else {
          hi = mid;
        }
      }
      return lo < advanceTableSize_[si] && table[lo].codepoint == codepoint;
    };

    // Map only new codepoints to global glyph indices.
    struct CpIdx {
      uint32_t codepoint;
      int32_t glyphIndex;
    };
    CpIdx* mappings = new (std::nothrow) CpIdx[cpCount];
    if (!mappings) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate mappings for style %u", si);
      totalMissed += cpCount;
      continue;
    }

    uint32_t validCount = 0;
    for (uint32_t i = 0; i < cpCount; i++) {
      if (isAlreadyCached(codepoints[i])) continue;
      if (oldCount + validCount >= MAX_ADVANCE_CODEPOINTS) break;
      int32_t idx = findGlobalGlyphIndex(s, codepoints[i]);
      if (idx >= 0) {
        mappings[validCount].codepoint = codepoints[i];
        mappings[validCount].glyphIndex = idx;
        validCount++;
      } else {
        totalMissed++;
      }
    }
    if (validCount == 0) {
      delete[] mappings;
      continue;
    }

    // Keep new entries separate until all SD reads succeed, then merge them
    // into the existing codepoint-sorted table.
    AdvanceEntry* additions = new (std::nothrow) AdvanceEntry[validCount];
    if (!additions) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate additions (%u entries) for style %u", validCount, si);
      delete[] mappings;
      continue;
    }
    for (uint32_t i = 0; i < validCount; i++) {
      additions[i].codepoint = mappings[i].codepoint;
      additions[i].advanceX = 0;
    }

    // Sort mappings by glyph index for sequential SD reads
    std::sort(mappings, mappings + validCount,
              [](const CpIdx& a, const CpIdx& b) { return a.glyphIndex < b.glyphIndex; });

    // Build a reverse map: for each mapping index, find its position in the
    // sorted-by-codepoint advance table. Since both are small and this runs
    // once per section build, O(n²) is acceptable.
    std::unique_ptr<uint32_t[]> tablePos(new (std::nothrow) uint32_t[validCount]);
    if (!tablePos) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to allocate tablePos for style %u", si);
      delete[] advanceTable_[si];
      advanceTable_[si] = nullptr;
      advanceTableSize_[si] = 0;
      delete[] mappings;
      continue;
    }
    for (uint32_t i = 0; i < validCount; i++) {
      // Binary search the advance table (sorted by codepoint) for this mapping's codepoint
      uint32_t lo = 0, hi = validCount;
      while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (additions[mid].codepoint < mappings[i].codepoint) {
          lo = mid + 1;
        } else {
          hi = mid;
        }
      }
      tablePos[i] = lo;
    }

    // Open file once, read advanceX for each glyph in index order
    FsFile file;
    if (!Storage.openFileForRead("SDCF", filePath_, file)) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to open .cpfont for style %u", si);
      delete[] additions;
      delete[] mappings;
      continue;
    }

    EpdGlyph tempGlyph;
    int32_t lastReadIndex = INT32_MIN;
    bool readSucceeded = true;
    for (uint32_t i = 0; i < validCount; i++) {
      int32_t gIdx = mappings[i].glyphIndex;
      uint32_t fileOff = s.glyphsFileOffset + static_cast<uint32_t>(gIdx) * sizeof(EpdGlyph);
      if (gIdx != lastReadIndex + 1) {
        file.seekSet(fileOff);
      }
      if (file.read(reinterpret_cast<uint8_t*>(&tempGlyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
        LOG_ERR("SDCF", "buildAdvanceTable: short glyph read (style %u, glyph %d)", si, gIdx);
        readSucceeded = false;
        break;
      }
      lastReadIndex = gIdx;
      additions[tablePos[i]].advanceX = tempGlyph.advanceX;
    }

    file.close();
    delete[] mappings;

    if (!readSucceeded) {
      delete[] additions;
      continue;
    }

    const uint32_t mergedCount = oldCount + validCount;
    AdvanceEntry* merged = new (std::nothrow) AdvanceEntry[mergedCount];
    if (!merged) {
      LOG_ERR("SDCF", "buildAdvanceTable: failed to grow table to %u entries for style %u", mergedCount, si);
      delete[] additions;
      continue;
    }
    uint32_t oldPos = 0;
    uint32_t addPos = 0;
    uint32_t mergedPos = 0;
    while (oldPos < oldCount && addPos < validCount) {
      if (advanceTable_[si][oldPos].codepoint < additions[addPos].codepoint) {
        merged[mergedPos++] = advanceTable_[si][oldPos++];
      } else {
        merged[mergedPos++] = additions[addPos++];
      }
    }
    while (oldPos < oldCount) merged[mergedPos++] = advanceTable_[si][oldPos++];
    while (addPos < validCount) merged[mergedPos++] = additions[addPos++];
    delete[] additions;
    delete[] advanceTable_[si];
    advanceTable_[si] = merged;
    advanceTableSize_[si] = mergedCount;

    LOG_DBG("SDCF", "Extended advance table: style %u, +%u = %u entries", si, validCount, mergedCount);
  }

  delete[] codepoints;

  stats_.prewarmTotalMs = millis() - startMs;
  return totalMissed;
}

// --- Stats ---

void SdCardFont::logStats(const char* label) {
#if defined(RENDER_PROFILE)
  LOG_INF("SDCF", "[%s] total=%ums sd_read=%ums seeks=%u metadata_seeks=%u bitmap_seeks=%u glyphs=%u bitmap=%u bytes",
          label, stats_.prewarmTotalMs, stats_.sdReadTimeMs, stats_.seekCount, stats_.metadataSeekCount,
          stats_.bitmapSeekCount, stats_.uniqueGlyphs, stats_.bitmapBytes);
  LOG_INF("SDCF",
          "[%s-io] metadata_reads=%u bitmap_reads=%u metadata_batches=%u bitmap_batches=%u metadata_batched=%u "
          "bitmap_batched=%u max_batch=%u bytes coalesced=%u read_ahead=%u bytes",
          label, stats_.metadataReadCount, stats_.bitmapReadCount, stats_.metadataBatchCount, stats_.bitmapBatchCount,
          stats_.metadataBatchedGlyphs, stats_.bitmapBatchedGlyphs, stats_.maxBatchBytes, stats_.coalescedGapCount,
          stats_.readAheadBytes);
  LOG_INF("SDCF", "[%s-arena] hits=%u bitmap_reuse=%u bitmap_grow=%u releases=%u", label, stats_.residentHitCount,
          stats_.bitmapArenaReuseCount, stats_.bitmapArenaGrowCount, stats_.retainedReleaseCount);
#else
  LOG_DBG("SDCF", "[%s] total=%ums sd_read=%ums seeks=%u glyphs=%u bitmap=%u bytes", label, stats_.prewarmTotalMs,
          stats_.sdReadTimeMs, stats_.seekCount, stats_.uniqueGlyphs, stats_.bitmapBytes);
#endif
}

void SdCardFont::resetStats() { stats_ = Stats{}; }

// --- Public accessors ---

EpdFont* SdCardFont::getEpdFont(uint8_t style) {
  if (style >= MAX_STYLES || !styles_[style].present) return nullptr;
  return &styles_[style].epdFont;
}

bool SdCardFont::hasStyle(uint8_t style) const { return style < MAX_STYLES && styles_[style].present; }

bool SdCardFont::hasCodepoint(uint32_t codepoint, uint8_t style) const {
  if (!loaded_) return false;
  const uint8_t resolvedStyle = resolveStyle(style);
  const auto& s = styles_[resolvedStyle];
  return s.fullIntervals && findGlobalGlyphIndex(s, codepoint) >= 0;
}

uint8_t SdCardFont::resolveStyle(uint8_t style) const {
  static const uint8_t kFallbacks[MAX_STYLES][MAX_STYLES] = {
      {EpdFontFamily::REGULAR, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::BOLD_ITALIC},
      {EpdFontFamily::BOLD, EpdFontFamily::REGULAR, EpdFontFamily::BOLD_ITALIC, EpdFontFamily::ITALIC},
      {EpdFontFamily::ITALIC, EpdFontFamily::REGULAR, EpdFontFamily::BOLD_ITALIC, EpdFontFamily::BOLD},
      {EpdFontFamily::BOLD_ITALIC, EpdFontFamily::BOLD, EpdFontFamily::ITALIC, EpdFontFamily::REGULAR},
  };

  const uint8_t styleBits = style & (MAX_STYLES - 1);
  for (uint8_t candidate : kFallbacks[styleBits]) {
    if (styles_[candidate].present) return candidate;
  }
  return EpdFontFamily::REGULAR;
}

uint8_t SdCardFont::resolveStyleMask(uint8_t styleMask) const {
  uint8_t resolvedMask = 0;
  for (uint8_t si = 0; si < MAX_STYLES; si++) {
    if (styleMask & (1 << si)) {
      resolvedMask |= static_cast<uint8_t>(1u << resolveStyle(si));
    }
  }
  return resolvedMask;
}

// --- On-demand glyph loading (overflow buffer) ---

const EpdGlyph* SdCardFont::onGlyphMiss(void* ctx, uint32_t codepoint) {
  auto* oc = static_cast<OverflowContext*>(ctx);
  auto* self = oc->self;
  uint8_t styleIdx = oc->styleIdx;

  if (!self->loaded_ || styleIdx >= MAX_STYLES || !self->styles_[styleIdx].present) return nullptr;
  const auto& s = self->styles_[styleIdx];
  if (!s.fullIntervals) return nullptr;

  // Check overflow cache first (matching both codepoint and style)
  for (uint32_t i = 0; i < self->overflowCount_; i++) {
    if (self->overflow_[i].codepoint == codepoint && self->overflow_[i].styleIdx == styleIdx) {
      return &self->overflow_[i].glyph;
    }
  }

  // Look up global glyph index via full intervals
  int32_t globalIdx = self->findGlobalGlyphIndex(s, codepoint);
  if (globalIdx < 0) return nullptr;

  // Pick overflow slot (ring buffer). Read into temporaries first so the
  // existing slot stays valid if SD I/O fails.
  uint32_t slot = self->overflowNext_;
  bool wasAtCapacity = (self->overflowCount_ == OVERFLOW_CAPACITY);
  if (!wasAtCapacity) {
    self->overflowCount_++;
  }
  self->overflowNext_ = (slot + 1) % OVERFLOW_CAPACITY;

  // Read glyph metadata into temporary
  FsFile file;
  if (!Storage.openFileForRead("SDCF", self->filePath_, file)) {
    LOG_ERR("SDCF", "Overflow: failed to open .cpfont");
    if (!wasAtCapacity) self->overflowCount_--;
    return nullptr;
  }

  EpdGlyph tempGlyph;
  uint32_t glyphFileOff = s.glyphsFileOffset + static_cast<uint32_t>(globalIdx) * sizeof(EpdGlyph);
  file.seekSet(glyphFileOff);
  if (file.read(reinterpret_cast<uint8_t*>(&tempGlyph), sizeof(EpdGlyph)) != sizeof(EpdGlyph)) {
    LOG_ERR("SDCF", "Overflow: failed to read glyph metadata for U+%04X style %u", codepoint, styleIdx);
    file.close();
    if (!wasAtCapacity) self->overflowCount_--;
    return nullptr;
  }

  // Read bitmap data into temporary (if any)
  uint8_t* tempBitmap = nullptr;
  if (tempGlyph.dataLength > 0) {
    tempBitmap = new (std::nothrow) uint8_t[tempGlyph.dataLength];
    if (!tempBitmap) {
      LOG_ERR("SDCF", "Overflow: failed to allocate %u bytes for U+%04X bitmap", tempGlyph.dataLength, codepoint);
      file.close();
      if (!wasAtCapacity) self->overflowCount_--;
      return nullptr;
    }
    file.seekSet(s.bitmapFileOffset + tempGlyph.dataOffset);
    if (file.read(tempBitmap, tempGlyph.dataLength) != static_cast<int>(tempGlyph.dataLength)) {
      LOG_ERR("SDCF", "Overflow: failed to read bitmap for U+%04X", codepoint);
      delete[] tempBitmap;
      file.close();
      if (!wasAtCapacity) self->overflowCount_--;
      return nullptr;
    }
  }

  file.close();

  // All reads succeeded — commit to slot (evict old entry if at capacity)
  if (wasAtCapacity) {
    delete[] self->overflow_[slot].bitmap;
  }
  self->overflow_[slot].glyph = tempGlyph;
  self->overflow_[slot].bitmap = tempBitmap;
  self->overflow_[slot].codepoint = codepoint;
  self->overflow_[slot].styleIdx = styleIdx;
#if !SD_FONT_DIAGNOSTICS
  // This can emit hundreds of rows per page. Suppress successful per-glyph
  // traces while collecting SFD measurements so USB serial does not drop
  // parts of the page-level diagnostic rows. Error logs above remain active.
  LOG_DBG("SDCF", "Overflow: loaded U+%04X style %u on demand (slot %u/%u)", codepoint, styleIdx, slot,
          OVERFLOW_CAPACITY);
#endif

  return &self->overflow_[slot].glyph;
}

bool SdCardFont::isOverflowGlyph(const EpdGlyph* glyph) const {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    if (&overflow_[i].glyph == glyph) return true;
  }
  return false;
}

const uint8_t* SdCardFont::getOverflowBitmap(const EpdGlyph* glyph) const {
  for (uint32_t i = 0; i < overflowCount_; i++) {
    if (&overflow_[i].glyph == glyph) {
      return overflow_[i].bitmap;
    }
  }
  return nullptr;
}

SdCardFont* SdCardFont::fromMissCtx(void* ctx) { return static_cast<OverflowContext*>(ctx)->self; }
