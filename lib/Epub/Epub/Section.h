#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Epub.h"

class Page;
class GfxRenderer;
class CssParser;

class Section {
 public:
  enum class CreateFailureReason { None, Cancelled, InsufficientMemory, StorageIo, Parse, CssUnavailable };

 private:
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  FsFile file;
  struct IncrementalBuild;
  std::shared_ptr<IncrementalBuild> incrementalBuild;
  bool freshTextOnlyBuild = false;
  CreateFailureReason lastCreateFailureReason = CreateFailureReason::None;
#if defined(CACHE_GENERATION_DIAGNOSTICS)
  // Set only by the full-cache callers that supply page-ready work.  The
  // compile-time switch removes this state from normal firmware builds.
  bool cacheGenerationDiagnosticsActive = false;
#endif

  void writeSectionFileHeader(int fontId, int tableFontId, float lineCompression, uint8_t extraParagraphSpacing,
                              uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                              bool hyphenationEnabled, bool firstLineIndent, uint8_t bookStyle, uint8_t imageRendering,
                              bool verticalMode, uint8_t charSpacing, uint8_t tateChuYokoMaxDigits);
  uint32_t onPageComplete(std::unique_ptr<Page> page);
  CssParser* loadEmbeddedCssForSection(uint8_t bookStyle, uint32_t fileSize, const std::string& htmlPath,
                                       bool requireComplete, bool& cssReady);
  bool streamSpineItemToTempHtml(const std::string& localPath, const std::string& tmpHtmlPath, uint32_t& fileSize,
                                 const std::function<bool()>& cancelFn = nullptr);
  bool readSectionOffsets(FsFile& file, uint32_t& lutOffset, uint32_t& anchorMapOffset) const;
  bool finalizeSectionFile(const std::vector<uint32_t>& lut,
                           const std::vector<std::pair<std::string, uint16_t>>& anchors,
                           const std::string& tmpSectionPath, CssParser* cssParser, uint32_t createSectionStart,
                           uint32_t parseBuildStart);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
      : epub(epub),
        spineIndex(spineIndex),
        renderer(renderer),
        filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {}
  ~Section();
  Section(const Section&) = delete;
  Section& operator=(const Section&) = delete;
  enum class BuildStep { Pending, Complete, Failed };
  BuildStep stepIncrementalBuild();
  void cancelIncrementalBuild();
#if defined(CACHE_STORAGE_FAULT_INJECTION)
  // Test-build hook. GenerateAllCacheActivity enables this only while its
  // synchronous batch operation is running.
  static void setCacheStorageFaultInjectionActive(bool active);
#endif
  bool loadSectionFile(int fontId, int tableFontId, float lineCompression, uint8_t extraParagraphSpacing,
                       uint8_t paragraphAlignment, uint16_t viewportWidth, uint16_t viewportHeight,
                       bool hyphenationEnabled, bool firstLineIndent, uint8_t bookStyle, uint8_t imageRendering,
                       bool verticalMode, uint8_t charSpacing, uint8_t tateChuYokoMaxDigits);
  bool clearCache() const;
  bool createSectionFile(int fontId, float lineCompression, uint8_t extraParagraphSpacing, uint8_t paragraphAlignment,
                         uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled, bool firstLineIndent,
                         uint8_t bookStyle, uint8_t imageRendering, bool verticalMode, uint8_t charSpacing,
                         uint8_t tateChuYokoMaxDigits, const std::function<void()>& popupFn = nullptr,
                         const int* headingFontIds = nullptr, int tableFontId = 0, const int* cssBodyFontIds = nullptr,
                         const std::function<void(uint16_t pagesDone, uint16_t estimatedPages)>& progressFn = nullptr,
                         const std::function<void(const Page&)>& pageReadyFn = nullptr,
                         const std::function<bool()>& cancelFn = nullptr, bool requireCompleteCss = true,
                         bool incremental = false);
  bool hasFreshTextOnlyBuild() const { return freshTextOnlyBuild; }
  CreateFailureReason getLastCreateFailureReason() const { return lastCreateFailureReason; }
  std::unique_ptr<Page> loadPageFromSectionFile();
  std::unique_ptr<Page> loadPageFromSectionFile(uint16_t pageNumber);

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;
};
