#pragma once

#include <Print.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"

class ZipFile;

class Epub {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Uniq cache key based on filepath
  std::string cachePath;
  // ReaderActivity obtains this before load() to resolve a per-book override.
  // Keep it for load() so the ZIP central directory is not scanned twice.
  mutable bool hasSourceFingerprint = false;
  mutable uint64_t cachedSourceFingerprint = 0;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  bool prepareSourceFingerprint(uint64_t fingerprint, bool& markerNeedsWrite);
  bool saveSourceFingerprint(uint64_t fingerprint) const;
  // CSS files
  std::vector<std::string> cssFiles;

  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true);
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  void discoverCssFilesFromZip();
  void parseCssFiles() const;

 public:
  enum class CacheGenerationStatus { NotGenerated, Resumable, Complete };

  explicit Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
    // create a cache key based on the filepath
    cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(this->filepath));
  }
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);
  // Shared by per-book and global cache management. This only removes a cache directory.
  static bool removeCacheDirectory(const std::string& cachePath);
  bool clearCache() const;
  void setupCacheDir() const;
  // A full-cache marker is published only after every section and image cache
  // has been generated.  Missing/invalid markers deliberately mean "resume".
  bool isFullCacheGenerated() const;
  // Read-only view of the full-cache contract. A cache directory holding only
  // reader metadata/progress is not generated cache; generated sections with
  // no completion marker are resumable.
  CacheGenerationStatus getCacheGenerationStatus() const;
  // Stable, content-free identifier based on the EPUB ZIP central directory.
  bool getSourceFingerprint(uint64_t* fingerprint) const;
  void clearFullCacheGeneratedMarker() const;
  void clearCachePromptSeenMarker() const;
  bool markFullCacheGenerated() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  std::string getCoverBmpPath(bool cropped = false) const;
  bool generateCoverBmp(bool cropped = false) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  bool generateThumbBmp(int height) const;
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  int resolveHrefToSpineIndex(const std::string& href) const;
  bool isPageProgressionRtl() const;
};
