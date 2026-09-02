#include "Section.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>

#include "Epub/css/CssParser.h"
#include "Epub/css/CssSelectorUsage.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr size_t SECTION_FONT_CODEPOINT_LIMIT = 1024;

void appendUtf8Codepoint(std::string& out, const uint32_t cp) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

bool collectSectionFontCodepoints(const std::string& htmlPath, std::string& uniqueText) {
  FsFile htmlFile;
  if (!Storage.openFileForRead("SCT", htmlPath, htmlFile)) return false;

  std::vector<uint32_t> codepoints;
  codepoints.reserve(SECTION_FONT_CODEPOINT_LIMIT);
  constexpr size_t SCAN_BUFFER_SIZE = 4096;
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[SCAN_BUFFER_SIZE]);
  if (!buffer) {
    htmlFile.close();
    return false;
  }
  uint32_t pendingCodepoint = 0;
  uint8_t pendingBytes = 0;

  const auto recordCodepoint = [&codepoints](const uint32_t cp) {
    const auto pos = std::lower_bound(codepoints.begin(), codepoints.end(), cp);
    if (pos == codepoints.end() || *pos != cp) codepoints.insert(pos, cp);
  };

  while (htmlFile.available() > 0 && codepoints.size() < SECTION_FONT_CODEPOINT_LIMIT) {
    const size_t bytesRead = htmlFile.read(buffer.get(), SCAN_BUFFER_SIZE);
    if (bytesRead == 0) break;
    for (size_t i = 0; i < bytesRead && codepoints.size() < SECTION_FONT_CODEPOINT_LIMIT; i++) {
      const uint8_t byte = buffer[i];
      if (pendingBytes == 0) {
        if (byte < 0x80) {
          recordCodepoint(byte);
        } else if ((byte & 0xE0) == 0xC0) {
          pendingCodepoint = byte & 0x1F;
          pendingBytes = 1;
        } else if ((byte & 0xF0) == 0xE0) {
          pendingCodepoint = byte & 0x0F;
          pendingBytes = 2;
        } else if ((byte & 0xF8) == 0xF0) {
          pendingCodepoint = byte & 0x07;
          pendingBytes = 3;
        }
      } else if ((byte & 0xC0) == 0x80) {
        pendingCodepoint = (pendingCodepoint << 6) | (byte & 0x3F);
        pendingBytes--;
        if (pendingBytes == 0) recordCodepoint(pendingCodepoint);
      } else {
        pendingCodepoint = 0;
        pendingBytes = 0;
      }
    }
  }
  htmlFile.close();

  uniqueText.clear();
  uniqueText.reserve(codepoints.size() * 3);
  for (const uint32_t cp : codepoints) appendUtf8Codepoint(uniqueText, cp);
  return !uniqueText.empty();
}

// Version 42: horizontal ruby stores its complete base-text span for centering.
// Version 43: full-page illustrations are isolated in both writing modes.
// Version 44: page layout reserves edge space for vertical and first-line horizontal ruby.
// Version 45: fast cache generation resolves ruby metrics before column placement.
// Version 49: book style, rather than the paragraph-alignment selector,
// determines whether EPUB text-align applies.
// Version 51: vertical h1/h2 after-block spacing changes stored column positions.
// Version 52: ruby base-text spans cannot split across vertical columns.
// Version 53: vertical ruby no longer adds per-column spacing beyond the common gutter.
// Version 54: horizontal line breaks apply Japanese head and tail kinsoku rules.
// Version 55: PNG dimensions no longer fail under low heap and omit images from persisted pages.
// Version 56: paragraph spacing stores its five-level preset rather than an on/off flag.
// Version 57: Aozora-download EPUBs turn full-width paragraph markers into blocks.
// Version 58: adjacent Aozora dialogue quotes begin a new paragraph.
// Version 59: Aozora dialogue after a sentence begins a new paragraph.
// Version 60: refresh vertical layout for bounded block spacing, voiced marks,
// punctuation, ruby, symbols, and halfwidth kana.
// Version 61: horizontal bars are stored as individual vertical punctuation cells.
// Version 62: parse and persist HTML <hr> rules in both writing directions.
// Version 63: leading halfwidth kana use a full vertical body cell.
// Version 64: halfwidth kana keep their own advance with fullwidth inter-cell
// spacing, and leading halfwidth kana use a half-em paragraph indent.
// Version 65: halfwidth kana use the same vertical cell pitch and paragraph
// indent as the surrounding Japanese body text.
// Version 66: halfwidth-only paragraphs use the kana's natural pitch and no
// first-line indent; U+FF70 no longer leaves a full-cell gap before its suffix.
// Version 67: halfwidth-only paragraphs use a half-line-height pitch and
// matching indent to avoid both overlap and full-cell gaps.
// Version 68: halfwidth-only pitch and indent increase to three fifths of the
// line height for a slightly lower line head and wider kana rhythm.
// Version 69: distinguish explicit HTML rules from heading separators so
// vertical h1/h2 underlines do not become page-height rules.
// Version 70: halfwidth-only vertical layout uses the active font's measured
// fullwidth advance instead of a line-height ratio.
// Version 71: explicit empty p/div paragraphs and consecutive <br> tags retain
// one invisible layout line instead of being discarded as an empty block.
// Version 72: explicit blank lines at a page or column head are discarded.
// Version 73: CSS emphasis on <ruby>/<rb> base text is preserved in word styles.
// Version 74: ruby parent and rb-specific emphasis have independent lifetimes.
// Version 75: SD-font advance-table misses retain their measured width instead
// of persisting overlapping vertical glyph positions after the table is full.
// Version 76: vertical EPUB layout persists ordinary HTML whitespace between
// Latin words instead of treating it as only a word boundary.
// Version 78: U+3000 ideographic spaces and basic directional arrows have
// updated vertical layout semantics. Inline character-size images are stored
// as sparse U+FFFC marker words, so their serialized layout also changed.
// Version 79: halfwidth punctuation U+FF61/U+FF64/U+FF65 and long mark
// U+FF70 use rotated vertical punctuation cells instead of kana metrics.
// Version 80: standalone voiced and semi-voiced marks occupy and center in
// their own vertical cells instead of attaching to an earlier base glyph.
// Version 81: vertical paragraph alignment is forced to justify (両端揃え) so
// first-line indent works regardless of the (otherwise ignored) setting.
// Version 82: skip auto first-line indent when a paragraph starts with an
// ideographic space (U+3000), so a book-encoded full-width-space indent is not
// doubled by the reader's own indent (applies to vertical + horizontal).
// Version 83: inline-image detection compares against the character-cell width
// (font size / emSize) instead of the "一" glyph advance, so it is font-family
// independent (sans-serif glyphs no longer overflow to a block image).
// Version 84: decode the standard &apos; HTML entity before pages are persisted.
// Version 85: retain complete ruby groups until their annotation is applied.
// Version 86: footnote href records retain up to 256 bytes.
// Version 87: retain complete ruby groups until their annotation is applied.
// Version 88: invalidate caches after correcting ruby close-tag handling.
// Version 89: balance all ruby close tags so enclosing emphasis cannot leak.
// Version 90: apply vertical column placement when completing a page.
// Version 91: reserve ruby leading for every horizontal line and vertical column.
// Version 92: use configured line/column spacing as the ruby-clearance
// baseline, adding only the remaining collision clearance.
// Version 93: supplementary CJK ideographs such as U+20B9F 𠮟 occupy
// individual upright vertical cells instead of a sideways text run.
// Version 94: expanded small-kana line-head prohibition changes vertical
// column breaks, so cached section layouts must be regenerated.
// Version 97: circled digits use full upright vertical character cells.
// Version 98: inline images retain a minimum vertical gutter at zero spacing.
// Version 99: enlarge that gutter to prevent overhanging glyph ink from touching images.
// Version 100: place inline images after the preceding glyph's visual cell.
constexpr uint8_t SECTION_FILE_VERSION = 100;
// Minimum free heap required before attempting to build section pages.
// Section building involves heavy allocations (Page, TextBlock, PageLine, etc.)
// and on ESP32 without C++ exceptions, allocation failure calls abort().
// Keep small XHTML files usable while still requiring more headroom for larger chapters.
constexpr size_t MIN_FREE_HEAP_FOR_TINY_SECTION_BUILD = 30 * 1024;    // 30KB
constexpr size_t MIN_FREE_HEAP_FOR_SMALL_SECTION_BUILD = 36 * 1024;   // 36KB
constexpr size_t MIN_FREE_HEAP_FOR_MEDIUM_SECTION_BUILD = 48 * 1024;  // 48KB
constexpr size_t MIN_FREE_HEAP_FOR_LARGE_SECTION_BUILD = 64 * 1024;   // 64KB
// Keep additional room for page objects, font metrics, and parser buffers when
// deciding whether a loaded external stylesheet can remain resident.
constexpr size_t CSS_SECTION_BUILD_RESERVE = 32 * 1024;  // 32KB
// XHTML size alone cannot predict a single long text block or a large glyph
// advance table.  Below this floor, release external rules and use inline CSS.
constexpr size_t MIN_FREE_HEAP_WITH_EXTERNAL_CSS = 96 * 1024;  // 96KB
// ZIP inflate streaming needs a 32KB sliding window plus a little room for file and temp allocations.
constexpr size_t MIN_MAX_ALLOC_FOR_SECTION_STREAM = 30 * 1024;  // 30KB
constexpr size_t MIN_FREE_HEAP_FOR_SECTION_STREAM = 30 * 1024;  // 30KB
constexpr size_t LUT_VALIDATION_BATCH_SIZE = 64;
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(uint8_t) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint8_t) + sizeof(uint8_t) + sizeof(bool) + sizeof(uint8_t) +  // charSpacing
                                 sizeof(uint32_t) + sizeof(uint32_t);

struct SectionHeader {
  uint8_t version = 0;
  int fontId = 0;
  float lineCompression = 0.0f;
  uint8_t extraParagraphSpacing = 0;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool firstLineIndent = false;
  uint8_t bookStyle = 0;
  uint8_t imageRendering = 0;
  bool verticalMode = false;
  uint8_t charSpacing = 0;
};

template <typename T>
bool readPodChecked(FsFile& file, T& value) {
  return file.read(&value, sizeof(value)) == static_cast<int>(sizeof(value));
}

double msToSeconds(const uint32_t elapsedMs) { return static_cast<double>(elapsedMs) / 1000.0; }

bool hasEnoughHeapForSectionStream() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  const bool ok = freeHeap >= MIN_FREE_HEAP_FOR_SECTION_STREAM && maxAllocHeap >= MIN_MAX_ALLOC_FOR_SECTION_STREAM;
  if (!ok) {
    LOG_ERR("SCT", "Insufficient heap for section stream (free=%u, maxAlloc=%u, need free>=%zu maxAlloc>=%zu)",
            freeHeap, maxAllocHeap, MIN_FREE_HEAP_FOR_SECTION_STREAM, MIN_MAX_ALLOC_FOR_SECTION_STREAM);
  }
  return ok;
}

size_t requiredHeapForSectionBuild(const uint32_t htmlSize) {
  if (htmlSize <= 2 * 1024) {
    return MIN_FREE_HEAP_FOR_TINY_SECTION_BUILD;
  }
  if (htmlSize <= 10 * 1024) {
    return MIN_FREE_HEAP_FOR_SMALL_SECTION_BUILD;
  }
  if (htmlSize <= 32 * 1024) {
    return MIN_FREE_HEAP_FOR_MEDIUM_SECTION_BUILD;
  }
  return MIN_FREE_HEAP_FOR_LARGE_SECTION_BUILD;
}

bool readSectionHeader(FsFile& file, SectionHeader& header) {
  return readPodChecked(file, header.version) && readPodChecked(file, header.fontId) &&
         readPodChecked(file, header.lineCompression) && readPodChecked(file, header.extraParagraphSpacing) &&
         readPodChecked(file, header.paragraphAlignment) && readPodChecked(file, header.viewportWidth) &&
         readPodChecked(file, header.viewportHeight) && readPodChecked(file, header.hyphenationEnabled) &&
         readPodChecked(file, header.firstLineIndent) && readPodChecked(file, header.bookStyle) &&
         readPodChecked(file, header.imageRendering) && readPodChecked(file, header.verticalMode) &&
         readPodChecked(file, header.charSpacing);
}

bool skipBoundedString(FsFile& file, const size_t fileSize) {
  uint32_t length = 0;
  if (!readPodChecked(file, length) || file.position() > fileSize || length > fileSize - file.position()) {
    return false;
  }
  return file.seek(file.position() + length);
}

bool validateSectionCache(FsFile& file, SectionHeader& header, uint16_t& pageCount, const char*& failureReason) {
  auto fail = [&failureReason](const char* reason) {
    failureReason = reason;
    return false;
  };
  const size_t fileSize = file.size();
  if (fileSize < HEADER_SIZE + sizeof(uint16_t) || fileSize > UINT32_MAX || !file.seek(0) ||
      !readSectionHeader(file, header)) {
    return fail("header");
  }

  uint32_t lutOffset = 0;
  uint32_t anchorMapOffset = 0;
  if (!readPodChecked(file, pageCount) || !readPodChecked(file, lutOffset) || !readPodChecked(file, anchorMapOffset) ||
      file.position() != HEADER_SIZE) {
    return fail("header offsets");
  }

  const uint32_t lutSize = static_cast<uint32_t>(pageCount) * sizeof(uint32_t);
  if (lutOffset < HEADER_SIZE || lutOffset > fileSize || lutSize > fileSize - lutOffset ||
      anchorMapOffset != lutOffset + lutSize || anchorMapOffset > fileSize - sizeof(uint16_t)) {
    return fail("LUT or anchor-map range");
  }

  uint32_t previousPageOffset = 0;
  uint32_t pageIndex = 0;
  uint32_t offsets[LUT_VALIDATION_BATCH_SIZE];
  if (!file.seek(lutOffset)) return fail("LUT seek");
  while (pageIndex < pageCount) {
    const size_t batchCount = std::min<size_t>(pageCount - pageIndex, LUT_VALIDATION_BATCH_SIZE);
    if (file.read(offsets, batchCount * sizeof(uint32_t)) != static_cast<int>(batchCount * sizeof(uint32_t))) {
      return fail("LUT read");
    }
    for (size_t i = 0; i < batchCount; ++i, ++pageIndex) {
      const uint32_t pageOffset = offsets[i];
      if (pageOffset < HEADER_SIZE || pageOffset >= lutOffset ||
          (pageIndex == 0 ? pageOffset != HEADER_SIZE : pageOffset <= previousPageOffset)) {
        return fail("page offset");
      }
      previousPageOffset = pageOffset;
    }
  }
  if (pageCount == 0 && lutOffset != HEADER_SIZE) {
    return fail("empty-section LUT");
  }

  if (!file.seek(anchorMapOffset)) return fail("anchor-map seek");
  uint16_t anchorCount = 0;
  if (!readPodChecked(file, anchorCount)) return fail("anchor count");
  for (uint16_t i = 0; i < anchorCount; ++i) {
    uint16_t page = 0;
    if (!skipBoundedString(file, fileSize) || !readPodChecked(file, page) || page >= pageCount) {
      return fail("anchor page");
    }
  }
  return file.position() == fileSize || fail("trailing data");
}
}  // namespace

#if defined(CACHE_GENERATION_DIAGNOSTICS)
namespace {
constexpr uint16_t kCacheDiagnosticsPageStart =
#ifdef CACHE_DIAGNOSTICS_PAGE_START
    CACHE_DIAGNOSTICS_PAGE_START;
#else
    60;
#endif
constexpr uint16_t kCacheDiagnosticsPageEnd =
#ifdef CACHE_DIAGNOSTICS_PAGE_END
    CACHE_DIAGNOSTICS_PAGE_END;
#else
    72;
#endif

bool isCacheDiagnosticsPage(const uint16_t page) {
  return page >= kCacheDiagnosticsPageStart && page <= kCacheDiagnosticsPageEnd;
}

struct PageLayoutStats {
  uint16_t textBlocks = 0;
  uint32_t characterCount = 0;
};

uint32_t utf8CodepointCount(const std::string& text) {
  uint32_t count = 0;
  for (const unsigned char byte : text) {
    if ((byte & 0xC0) != 0x80) ++count;
  }
  return count;
}

struct VerticalWordPosition {
  int x;
  int y;
  uint16_t block;
  uint16_t word;
  const char* text;
};

PageLayoutStats logVerticalLayoutDiagnostics(const Page& page, const int spineIndex, const uint16_t pageIndex) {
  PageLayoutStats stats;
  // A page has a bounded number of glyphs.  Avoid allocating a coordinate map
  // while inspecting it: this diagnostic runs only in the debug cache build.
  std::vector<VerticalWordPosition> positions;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& block = static_cast<const PageLine&>(*element).getBlock();
    if (!block) continue;
    ++stats.textBlocks;

    const auto& words = block->getWords();
    for (const auto& word : words) stats.characterCount += utf8CodepointCount(word);
    if (!block->getIsVertical()) continue;

    const auto& xs = block->getWordXpos();
    const auto& ys = block->getWordYpos();
    if (xs.size() != words.size() || ys.size() != words.size()) {
      LOG_ERR("CDIAG", "VERT_SIZE_MISMATCH spine=%d page=%u words=%u x=%u y=%u", spineIndex, pageIndex,
              static_cast<unsigned>(words.size()), static_cast<unsigned>(xs.size()), static_cast<unsigned>(ys.size()));
      continue;
    }
    for (size_t i = 0; i < words.size(); ++i) {
      const int x = element->xPos + xs[i];
      const int y = element->yPos + ys[i];
      for (const auto& previous : positions) {
        if (x != previous.x || y != previous.y) continue;
        LOG_ERR("CDIAG",
                "VERT_DUPLICATE spine=%d page=%u block=%u word=%u previousBlock=%u previousWord=%u x=%d y=%d text=%s "
                "prev=%s",
                spineIndex, pageIndex, static_cast<unsigned>(stats.textBlocks - 1), static_cast<unsigned>(i),
                previous.block, previous.word, x, y, words[i].c_str(), previous.text);
        break;
      }
      positions.push_back(
          {x, y, static_cast<uint16_t>(stats.textBlocks - 1), static_cast<uint16_t>(i), words[i].c_str()});
    }
  }
  return stats;
}
}  // namespace
#endif

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
#if defined(CACHE_GENERATION_DIAGNOSTICS)
  PageLayoutStats layoutStats;
  const bool logPage = cacheGenerationDiagnosticsActive && isCacheDiagnosticsPage(pageCount);
  if (logPage) layoutStats = logVerticalLayoutDiagnostics(*page, spineIndex, pageCount);
#endif
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", pageCount);
#if defined(CACHE_GENERATION_DIAGNOSTICS)
  if (logPage) {
    LOG_DBG("CDIAG",
            "PAGE spine=%d page=%u range=%lu..%lu record=%lu charsUtf8=%lu textBlocks=%u elements=%u free=%u min=%u "
            "maxAlloc=%u",
            spineIndex, pageCount, static_cast<unsigned long>(position), static_cast<unsigned long>(file.position()),
            static_cast<unsigned long>(file.position() - position),
            static_cast<unsigned long>(layoutStats.characterCount), layoutStats.textBlocks,
            static_cast<unsigned>(page->elements.size()), ESP.getFreeHeap(), ESP.getMinFreeHeap(),
            ESP.getMaxAllocHeap());
  }
#endif

  pageCount++;
  return position;
}

void Section::writeSectionFileHeader(const int fontId, const float lineCompression, const uint8_t extraParagraphSpacing,
                                     const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                     const uint16_t viewportHeight, const bool hyphenationEnabled,
                                     const bool firstLineIndent, const uint8_t bookStyle, const uint8_t imageRendering,
                                     const bool verticalMode, const uint8_t charSpacing) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                   sizeof(extraParagraphSpacing) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(pageCount) + sizeof(hyphenationEnabled) +
                                   sizeof(firstLineIndent) + sizeof(bookStyle) + sizeof(imageRendering) +
                                   sizeof(verticalMode) + sizeof(charSpacing) + sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  serialization::writePod(file, SECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, lineCompression);
  serialization::writePod(file, extraParagraphSpacing);
  serialization::writePod(file, paragraphAlignment);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, hyphenationEnabled);
  serialization::writePod(file, firstLineIndent);
  serialization::writePod(file, bookStyle);
  serialization::writePod(file, imageRendering);
  serialization::writePod(file, verticalMode);
  serialization::writePod(file, charSpacing);
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0, patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for anchor map offset (patched later)
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const uint8_t extraParagraphSpacing,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled, const bool firstLineIndent,
                              const uint8_t bookStyle, const uint8_t imageRendering, const bool verticalMode,
                              const uint8_t charSpacing) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  SectionHeader header;
  uint16_t validatedPageCount = 0;
  const char* validationFailure = "unknown";
  if (!validateSectionCache(file, header, validatedPageCount, validationFailure)) {
    file.close();
    LOG_ERR("SCT", "Section cache validation failed: %s", validationFailure);
    epub->clearCachePromptSeenMarker();
    clearCache();
    return false;
  }
  if (header.version != SECTION_FILE_VERSION) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: Unknown version %u", header.version);
    epub->clearCachePromptSeenMarker();
    clearCache();
    return false;
  }

  const bool parametersMatch =
      fontId == header.fontId && lineCompression == header.lineCompression &&
      extraParagraphSpacing == header.extraParagraphSpacing && paragraphAlignment == header.paragraphAlignment &&
      viewportWidth == header.viewportWidth && viewportHeight == header.viewportHeight &&
      hyphenationEnabled == header.hyphenationEnabled && firstLineIndent == header.firstLineIndent &&
      bookStyle == header.bookStyle && imageRendering == header.imageRendering && verticalMode == header.verticalMode &&
      charSpacing == header.charSpacing;
  if (!parametersMatch) {
#if defined(CACHE_GENERATION_DIAGNOSTICS)
    if (fontId != header.fontId)
      LOG_DBG("CDIAG", "PARAM_MISMATCH spine=%d field=fontId current=%d cached=%d", spineIndex, fontId, header.fontId);
    if (lineCompression != header.lineCompression)
      LOG_DBG("CDIAG", "PARAM_MISMATCH spine=%d field=lineCompression current=%.3f cached=%.3f", spineIndex,
              static_cast<double>(lineCompression), static_cast<double>(header.lineCompression));
    if (extraParagraphSpacing != header.extraParagraphSpacing)
      LOG_DBG("CDIAG", "PARAM_MISMATCH spine=%d field=extraParagraphSpacing current=%u cached=%u", spineIndex,
              static_cast<unsigned>(extraParagraphSpacing), static_cast<unsigned>(header.extraParagraphSpacing));
    if (paragraphAlignment != header.paragraphAlignment)
      LOG_DBG("CDIAG", "PARAM_MISMATCH spine=%d field=paragraphAlignment current=%u cached=%u", spineIndex,
              static_cast<unsigned>(paragraphAlignment), static_cast<unsigned>(header.paragraphAlignment));
    if (viewportWidth != header.viewportWidth)
      LOG_DBG("CDIAG", "PARAM_MISMATCH spine=%d field=viewportWidth current=%u cached=%u", spineIndex,
              static_cast<unsigned>(viewportWidth), static_cast<unsigned>(header.viewportWidth));
    if (viewportHeight != header.viewportHeight)
      LOG_DBG("CDIAG", "PARAM_MISMATCH spine=%d field=viewportHeight current=%u cached=%u", spineIndex,
              static_cast<unsigned>(viewportHeight), static_cast<unsigned>(header.viewportHeight));
    if (hyphenationEnabled != header.hyphenationEnabled)
      LOG_DBG("CDIAG", "PARAM_MISMATCH spine=%d field=hyphenationEnabled current=%u cached=%u", spineIndex,
              static_cast<unsigned>(hyphenationEnabled), static_cast<unsigned>(header.hyphenationEnabled));
    if (firstLineIndent != header.firstLineIndent)
      LOG_DBG("CDIAG", "PARAM_MISMATCH spine=%d field=firstLineIndent current=%u cached=%u", spineIndex,
              static_cast<unsigned>(firstLineIndent), static_cast<unsigned>(header.firstLineIndent));
    if (bookStyle != header.bookStyle)
      LOG_DBG("CDIAG", "PARAM_MISMATCH spine=%d field=bookStyle current=%u cached=%u", spineIndex,
              static_cast<unsigned>(bookStyle), static_cast<unsigned>(header.bookStyle));
    if (imageRendering != header.imageRendering)
      LOG_DBG("CDIAG", "PARAM_MISMATCH spine=%d field=imageRendering current=%u cached=%u", spineIndex,
              static_cast<unsigned>(imageRendering), static_cast<unsigned>(header.imageRendering));
    if (verticalMode != header.verticalMode)
      LOG_DBG("CDIAG", "PARAM_MISMATCH spine=%d field=verticalMode current=%u cached=%u", spineIndex,
              static_cast<unsigned>(verticalMode), static_cast<unsigned>(header.verticalMode));
    if (charSpacing != header.charSpacing)
      LOG_DBG("CDIAG", "PARAM_MISMATCH spine=%d field=charSpacing current=%u cached=%u", spineIndex,
              static_cast<unsigned>(charSpacing), static_cast<unsigned>(header.charSpacing));
#endif
    file.close();
    LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
    epub->clearCachePromptSeenMarker();
    clearCache();
    return false;
  }

  pageCount = validatedPageCount;
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages", pageCount);
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  // Removing or discovering a missing section makes the book-level completion
  // marker stale. A later successful full-cache run will publish it again.
  epub->clearFullCacheGeneratedMarker();

  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

CssParser* Section::loadEmbeddedCssForSection(const uint8_t bookStyle, const uint32_t fileSize,
                                              const std::string& htmlPath) {
  if (bookStyle == 0) {
    return nullptr;
  }

  CssParser* cssParser = epub->getCssParser();
  if (!cssParser) {
    return nullptr;
  }

  const size_t minFreeHeap =
      std::max(MIN_FREE_HEAP_WITH_EXTERNAL_CSS, requiredHeapForSectionBuild(fileSize) + CSS_SECTION_BUILD_RESERVE);
  CssSelectorUsage usage;
  const bool scanned = usage.scanHtmlFile(htmlPath);
  if (!cssParser->loadFromCache(minFreeHeap, scanned ? &usage : nullptr)) {
    LOG_INF("SCT", "CSS cache unavailable or skipped; continuing without external rules");
    return nullptr;
  }

  LOG_DBG("SCT", "CSS cache loaded: rules=%zu, free=%u, maxAlloc=%u", cssParser->ruleCount(), ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());

  if (cssParser->empty()) {
    LOG_DBG("SCT", "CSS cache has no rules, skipping stylesheet lookup for this section");
    cssParser->clear();
    return nullptr;
  }

  if (ESP.getFreeHeap() < minFreeHeap) {
    LOG_INF("SCT", "Skipping external CSS for section build (rules=%zu, free=%u, need>=%zu, html=%lu)",
            cssParser->ruleCount(), ESP.getFreeHeap(), minFreeHeap, static_cast<unsigned long>(fileSize));
    cssParser->clear();
    return nullptr;
  }

  return cssParser;
}

bool Section::streamSpineItemToTempHtml(const std::string& localPath, const std::string& tmpHtmlPath,
                                        uint32_t& fileSize) {
  // Retry logic for SD card timing issues
  bool success = false;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
      delay(50);  // Brief delay before retry
    }

    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }

    FsFile tmpHtml;
    if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }

    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    tmpHtml.close();

    if (!success && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
      LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
    }
  }

  return success;
}

bool Section::readSectionOffsets(FsFile& file, uint32_t& lutOffset, uint32_t& anchorMapOffset) const {
  return file.seek(HEADER_SIZE - sizeof(uint32_t) * 2) && readPodChecked(file, lutOffset) &&
         readPodChecked(file, anchorMapOffset);
}

bool Section::finalizeSectionFile(const std::vector<uint32_t>& lut,
                                  const std::vector<std::pair<std::string, uint16_t>>& anchors,
                                  const std::string& tmpSectionPath, CssParser* cssParser,
                                  const uint32_t createSectionStart, const uint32_t parseBuildStart) {
  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  for (const uint32_t& pos : lut) {
    if (pos == 0) {
      hasFailedLutRecords = true;
      break;
    }
    serialization::writePod(file, pos);
  }

  if (hasFailedLutRecords) {
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }

  const uint32_t anchorMapOffset = file.position();
  uint16_t validAnchorCount = 0;
  size_t droppedAnchorCount = 0;
  for (const auto& [anchor, page] : anchors) {
    (void)anchor;
    if (page < pageCount && validAnchorCount < UINT16_MAX) {
      ++validAnchorCount;
    } else {
      ++droppedAnchorCount;
    }
  }
  if (droppedAnchorCount > 0) {
    LOG_DBG("SCT", "Dropped %u anchor(s) without a generated page", static_cast<unsigned>(droppedAnchorCount));
  }
  serialization::writePod(file, validAnchorCount);
  uint16_t anchorsWritten = 0;
  for (const auto& [anchor, page] : anchors) {
    if (page >= pageCount || anchorsWritten == validAnchorCount) continue;
    serialization::writeString(file, anchor);
    serialization::writePod(file, page);
    ++anchorsWritten;
  }

  file.seek(HEADER_SIZE - sizeof(uint32_t) * 2 - sizeof(pageCount));
  serialization::writePod(file, pageCount);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  file.close();
  if (cssParser) {
    cssParser->clear();
  }

  FsFile validationFile;
  SectionHeader validatedHeader;
  uint16_t validatedPageCount = 0;
  const char* validationFailure = "open";
  size_t validationFileSize = 0;
  if (!Storage.openFileForRead("SCT", tmpSectionPath, validationFile) ||
      !validateSectionCache(validationFile, validatedHeader, validatedPageCount, validationFailure) ||
      validatedPageCount != pageCount) {
    validationFileSize = validationFile.size();
    if (validatedPageCount != pageCount && strcmp(validationFailure, "open") == 0) {
      validationFailure = "page count";
    }
    validationFile.close();
    LOG_ERR("SCT", "Generated section cache failed validation: %s (size=%lu, pages=%u, validated=%u)",
            validationFailure, static_cast<unsigned long>(validationFileSize), pageCount, validatedPageCount);
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }
  validationFile.close();

  const uint32_t parseBuildElapsedMs = millis() - parseBuildStart;
  const uint32_t totalElapsedMs = millis() - createSectionStart;
  LOG_DBG("SCT", "Section %d page build took %lu ms (%.2f s)", spineIndex, parseBuildElapsedMs,
          msToSeconds(parseBuildElapsedMs));
  LOG_DBG("SCT", "Section %d total create took %lu ms (%.2f s)", spineIndex, totalElapsedMs,
          msToSeconds(totalElapsedMs));

  if (Storage.exists(filePath.c_str()) && !Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to remove old section cache before rename");
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }

  if (!Storage.rename(tmpSectionPath.c_str(), filePath.c_str())) {
    LOG_ERR("SCT", "Failed to finalize section cache: %s -> %s", tmpSectionPath.c_str(), filePath.c_str());
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }

  return true;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const uint8_t extraParagraphSpacing,
                                const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                const uint16_t viewportHeight, const bool hyphenationEnabled,
                                const bool firstLineIndent, const uint8_t bookStyle, const uint8_t imageRendering,
                                const bool verticalMode, const uint8_t charSpacing,
                                const std::function<void()>& popupFn, const int* headingFontIds, const int tableFontId,
                                const int* cssBodyFontIds,
                                const std::function<void(uint16_t pagesDone, uint16_t estimatedPages)>& progressFn,
                                const std::function<void(const Page&)>& pageReadyFn,
                                const std::function<bool()>& cancelFn) {
  const uint32_t createSectionStart = millis();
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";
  const auto tmpSectionPath = filePath + ".tmp";
#if defined(CACHE_GENERATION_DIAGNOSTICS)
  cacheGenerationDiagnosticsActive = static_cast<bool>(pageReadyFn);
  if (cacheGenerationDiagnosticsActive) {
    LOG_DBG("CDIAG", "BEGIN spine=%d href=%s cache=%s pageWindow=%u..%u free=%u min=%u maxAlloc=%u", spineIndex,
            localPath.c_str(), filePath.c_str(), kCacheDiagnosticsPageStart, kCacheDiagnosticsPageEnd,
            ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
  }
#endif

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // ZIP inflation needs a 32KB contiguous buffer. Check this before we spend
  // memory on CSS/cache setup or temp-file retries.
  if (!hasEnoughHeapForSectionStream()) {
    return false;
  }

  bool success = false;
  uint32_t fileSize = 0;
  success = streamSpineItemToTempHtml(localPath, tmpHtmlPath, fileSize);

  if (!success) {
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    return false;
  }

  const uint32_t streamElapsedMs = millis() - createSectionStart;
  LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes) in %lu ms (%.2f s)", tmpHtmlPath.c_str(), fileSize,
          streamElapsedMs, msToSeconds(streamElapsedMs));

  if (Storage.exists(tmpSectionPath.c_str())) {
    Storage.remove(tmpSectionPath.c_str());
  }

  if (!Storage.openFileForWrite("SCT", tmpSectionPath, file)) {
    return false;
  }
  writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled, firstLineIndent, bookStyle, imageRendering, verticalMode,
                         charSpacing);
  std::vector<uint32_t> lut = {};
  std::vector<uint16_t> imagePages = {};

  CssParser* cssParser = loadEmbeddedCssForSection(bookStyle, fileSize, tmpHtmlPath);

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  // Pre-check heap before heavy allocation work.
  // On ESP32 without C++ exceptions, new/make_shared call abort() on failure.
  const uint32_t freeHeapBeforeBuild = ESP.getFreeHeap();
  const uint32_t maxAllocHeapBeforeBuild = ESP.getMaxAllocHeap();
  const size_t requiredHeapBeforeBuild = requiredHeapForSectionBuild(fileSize);
  // CSS parsing and font setup can fragment the heap after the earlier ZIP-stream
  // check.  The parser's initial ParsedText reserves need one sizeable contiguous
  // allocation, so total free heap alone is not a safe admission test here.
  if (freeHeapBeforeBuild < requiredHeapBeforeBuild || maxAllocHeapBeforeBuild < MIN_MAX_ALLOC_FOR_SECTION_STREAM) {
    LOG_ERR("SCT",
            "Insufficient heap for section build (free=%u, maxAlloc=%u, need free>=%zu maxAlloc>=%zu, html=%lu), "
            "aborting gracefully",
            freeHeapBeforeBuild, maxAllocHeapBeforeBuild, requiredHeapBeforeBuild, MIN_MAX_ALLOC_FOR_SECTION_STREAM,
            static_cast<unsigned long>(fileSize));
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    Storage.remove(tmpHtmlPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  LOG_DBG("SCT", "Section build heap check passed (free=%u, maxAlloc=%u, need free>=%zu maxAlloc>=%zu, html=%lu)",
          freeHeapBeforeBuild, maxAllocHeapBeforeBuild, requiredHeapBeforeBuild, MIN_MAX_ALLOC_FOR_SECTION_STREAM,
          static_cast<unsigned long>(fileSize));

  const uint32_t parseBuildStart = millis();
  renderer.resetSdCardAdvanceBuildTiming();
  if (renderer.isSdCardFont(fontId)) {
    std::string sectionFontText;
    if (collectSectionFontCodepoints(tmpHtmlPath, sectionFontText)) {
      renderer.ensureSdCardFontReady(fontId, sectionFontText.c_str());
    }
  }
  const uint32_t estimatedBytesPerPage = verticalMode ? 700 : 3072;
  const uint16_t estimatedPages =
      std::max<uint16_t>(4, static_cast<uint16_t>((fileSize + estimatedBytesPerPage - 1) / estimatedBytesPerPage));
  ChapterHtmlSlimParser visitor(
      epub, tmpHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
      viewportHeight, hyphenationEnabled, firstLineIndent,
      [this, &lut, &imagePages, &progressFn, &pageReadyFn, estimatedPages](std::unique_ptr<Page> page) {
        if (pageReadyFn && page->hasImages()) imagePages.push_back(pageCount);
        lut.emplace_back(this->onPageComplete(std::move(page)));
        if (progressFn) {
          progressFn(pageCount, estimatedPages);
        }
      },
      bookStyle, contentBase, imageBasePath, imageRendering, popupFn, cssParser, headingFontIds, tableFontId,
      verticalMode, cssBodyFontIds, cancelFn);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  success = visitor.parseAndBuildPages();
  LOG_INF("SCT", "Section %d parse/build=%lu ms, SD advance tables=%lu ms (%lu builds)", spineIndex,
          millis() - parseBuildStart, renderer.getSdCardAdvanceBuildMs(), renderer.getSdCardAdvanceBuildCalls());

  Storage.remove(tmpHtmlPath.c_str());
  if (!success) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  const auto& anchors = visitor.getAnchors();
  if (!finalizeSectionFile(lut, anchors, tmpSectionPath, cssParser, createSectionStart, parseBuildStart)) {
    return false;
  }

  // CSS and parser allocations are now released. Reload only pages that contain
  // images so optional cache work has enough contiguous heap without making
  // text-only books scan every persisted page.
  if (pageReadyFn) {
    for (const auto pageIndex : imagePages) {
      auto page = loadPageFromSectionFile(pageIndex);
      if (page) pageReadyFn(*page);
    }
  }
  return true;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() { return loadPageFromSectionFile(currentPage); }

std::unique_ptr<Page> Section::loadPageFromSectionFile(const uint16_t pageNumber) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return nullptr;
  }

  if (pageNumber >= pageCount) {
    file.close();
    return nullptr;
  }

  uint32_t lutOffset = 0;
  uint32_t anchorMapOffset = 0;
  readSectionOffsets(file, lutOffset, anchorMapOffset);
  file.seek(lutOffset + sizeof(uint32_t) * pageNumber);
  uint32_t pagePos = 0;
  serialization::readPod(file, pagePos);
  file.seek(pagePos);

  auto page = Page::deserialize(file);
  file.close();
  return page;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  uint32_t lutOffset = 0;
  uint32_t anchorMapOffset = 0;
  readSectionOffsets(f, lutOffset, anchorMapOffset);
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    f.close();
    return std::nullopt;
  }

  f.seek(anchorMapOffset);
  uint16_t count;
  serialization::readPod(f, count);
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    serialization::readString(f, key);
    serialization::readPod(f, page);
    if (key == anchor) {
      f.close();
      return page;
    }
  }

  f.close();
  return std::nullopt;
}

// Paragraph-level KOSync (upstream PR #1686) is not supported in this fork.
// The section file format here does not carry paragraph indices in its LUT.
std::optional<uint16_t> Section::getPageForParagraphIndex(uint16_t /*pIndex*/) const { return std::nullopt; }
