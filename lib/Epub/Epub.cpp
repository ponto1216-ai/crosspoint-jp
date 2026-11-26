#include "Epub.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <ZipFile.h>
#include <Txt.h>
#include <new>

#include "Epub/parsers/ContainerParser.h"
#include "Epub/parsers/ContentOpfParser.h"
#include "Epub/parsers/TocNavParser.h"
#include "Epub/parsers/TocNcxParser.h"

namespace {
constexpr uint8_t SOURCE_FINGERPRINT_VERSION = 1;
constexpr char sourceFingerprintFile[] = "/source.fingerprint";
constexpr char sourceFingerprintTmpFile[] = "/source.fingerprint.tmp";
constexpr uint8_t FULL_CACHE_MARKER_VERSION = 1;
constexpr char fullCacheMarkerFile[] = "/.full_cache_complete";
constexpr char fullCacheMarkerTmpFile[] = "/.full_cache_complete.tmp";
constexpr char cachePromptSeenMarkerFile[] = "/.cache_prompt_seen";
constexpr char legacyNoCachePromptMarkerFile[] = "/.no_cache_prompt";
constexpr size_t MAX_SVG_COVER_BYTES = 128 * 1024;

bool isRasterImageReference(const std::string_view reference) {
  return FsHelpers::hasPngExtension(reference) || FsHelpers::hasJpgExtension(reference);
}

bool findAttributeValue(const std::string_view tag, const std::string_view attribute, std::string& value) {
  size_t searchFrom = 0;
  while (true) {
    const size_t attributeStart = tag.find(attribute, searchFrom);
    if (attributeStart == std::string_view::npos) return false;
    const bool validStart =
        attributeStart == 0 ||
        (tag[attributeStart - 1] != ':' && tag[attributeStart - 1] != '-' && tag[attributeStart - 1] != '_' &&
         !(tag[attributeStart - 1] >= 'a' && tag[attributeStart - 1] <= 'z') &&
         !(tag[attributeStart - 1] >= 'A' && tag[attributeStart - 1] <= 'Z'));
    size_t cursor = attributeStart + attribute.size();
    while (cursor < tag.size() &&
           (tag[cursor] == ' ' || tag[cursor] == '\t' || tag[cursor] == '\r' || tag[cursor] == '\n')) {
      ++cursor;
    }
    if (!validStart || cursor >= tag.size() || tag[cursor] != '=') {
      searchFrom = attributeStart + attribute.size();
      continue;
    }
    ++cursor;
    while (cursor < tag.size() &&
           (tag[cursor] == ' ' || tag[cursor] == '\t' || tag[cursor] == '\r' || tag[cursor] == '\n')) {
      ++cursor;
    }
    if (cursor >= tag.size() || (tag[cursor] != '\'' && tag[cursor] != '"')) return false;
    const char quote = tag[cursor++];
    const size_t valueEnd = tag.find(quote, cursor);
    if (valueEnd == std::string_view::npos) return false;
    value.assign(tag.substr(cursor, valueEnd - cursor));
    return true;
  }
}

bool findRasterCoverImageReference(const std::string_view markup, std::string& reference) {
  size_t searchFrom = 0;
  while (true) {
    const size_t tagStart = markup.find('<', searchFrom);
    if (tagStart == std::string_view::npos) return false;
    const size_t tagEnd = markup.find('>', tagStart + 1);
    if (tagEnd == std::string_view::npos) return false;
    const std::string_view tag = markup.substr(tagStart, tagEnd - tagStart + 1);
    const bool isImage = tag.size() >= 6 && tag.substr(0, 6) == "<image";
    const bool isNamespacedImage = tag.size() >= 10 && tag.substr(0, 10) == "<svg:image";
    const bool isHtmlImage = tag.size() >= 4 && tag.substr(0, 4) == "<img" &&
                             (tag.size() == 4 || tag[4] == ' ' || tag[4] == '\t' || tag[4] == '\r' || tag[4] == '\n' ||
                              tag[4] == '>' || tag[4] == '/');
    if (isImage || isNamespacedImage || isHtmlImage) {
      std::string candidate;
      const bool hasReference = isHtmlImage ? findAttributeValue(tag, "src", candidate)
                                            : (findAttributeValue(tag, "xlink:href", candidate) ||
                                               findAttributeValue(tag, "href", candidate));
      if (hasReference && isRasterImageReference(candidate)) {
        reference = std::move(candidate);
        return true;
      }
    }
    searchFrom = tagEnd + 1;
  }
}

std::string resolveSvgCoverImageHref(const Epub& epub, const std::string& coverHref) {
  if (isRasterImageReference(coverHref)) return coverHref;
  if (!FsHelpers::checkFileExtension(coverHref, ".svg")) return {};

  size_t svgSize = 0;
  if (!epub.getItemSize(coverHref, &svgSize) || svgSize == 0 || svgSize > MAX_SVG_COVER_BYTES) {
    LOG_ERR("EBP", "SVG cover is unavailable or too large: %s", coverHref.c_str());
    return {};
  }
  uint8_t* svgData = epub.readItemContentsToBytes(coverHref, &svgSize, true);
  if (!svgData) return {};
  const std::string_view svg(reinterpret_cast<char*>(svgData), svgSize);
  std::string rasterReference;
  const bool found = findRasterCoverImageReference(svg, rasterReference);
  free(svgData);
  if (!found) return {};

  const size_t lastSlash = coverHref.rfind('/');
  const std::string basePath = lastSlash == std::string::npos ? "" : coverHref.substr(0, lastSlash + 1);
  const std::string resolved = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(basePath + rasterReference));
  size_t rasterSize = 0;
  return epub.getItemSize(resolved, &rasterSize) && rasterSize > 0 ? resolved : std::string{};
}
}  // namespace

bool Epub::prepareSourceFingerprint(const uint64_t fingerprint, bool& markerNeedsWrite) {
  markerNeedsWrite = false;
  const std::string markerPath = cachePath + sourceFingerprintFile;
  if (!Storage.exists(markerPath.c_str())) {
    // Adopt existing pre-fingerprint caches without a one-time forced rebuild.
    markerNeedsWrite = true;
    return true;
  }

  FsFile marker;
  if (!Storage.openFileForRead("EBP", markerPath, marker)) {
    LOG_ERR("EBP", "Could not read EPUB source fingerprint");
    return false;
  }

  uint8_t version = 0;
  uint64_t cachedFingerprint = 0;
  const bool valid = marker.read(&version, sizeof(version)) == sizeof(version) &&
                     marker.read(&cachedFingerprint, sizeof(cachedFingerprint)) == sizeof(cachedFingerprint) &&
                     version == SOURCE_FINGERPRINT_VERSION;
  marker.close();

  if (valid && cachedFingerprint == fingerprint) {
    return true;
  }

  LOG_INF("EBP", "EPUB source changed or fingerprint is invalid; rebuilding per-book cache");
  if (!clearCache()) {
    return false;
  }
  setupCacheDir();
  markerNeedsWrite = true;
  return true;
}

bool Epub::saveSourceFingerprint(const uint64_t fingerprint) const {
  const std::string markerPath = cachePath + sourceFingerprintFile;
  const std::string tmpPath = cachePath + sourceFingerprintTmpFile;
  Storage.remove(tmpPath.c_str());

  FsFile marker;
  if (!Storage.openFileForWrite("EBP", tmpPath, marker)) {
    LOG_ERR("EBP", "Could not create EPUB source fingerprint");
    return false;
  }

  const uint8_t version = SOURCE_FINGERPRINT_VERSION;
  const bool written = marker.write(&version, sizeof(version)) == sizeof(version) &&
                       marker.write(&fingerprint, sizeof(fingerprint)) == sizeof(fingerprint);
  marker.flush();
  marker.close();
  if (!written) {
    Storage.remove(tmpPath.c_str());
    LOG_ERR("EBP", "Could not write EPUB source fingerprint");
    return false;
  }

  Storage.remove(markerPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), markerPath.c_str())) {
    Storage.remove(tmpPath.c_str());
    LOG_ERR("EBP", "Could not finalize EPUB source fingerprint");
    return false;
  }
  return true;
}

bool Epub::isFullCacheGenerated() const {
  const std::string markerPath = cachePath + fullCacheMarkerFile;
  // Missing completion markers are normal for books that have not had a full
  // cache generated. Avoid reporting that expected state as a failed open.
  if (!Storage.exists(markerPath.c_str())) return false;

  FsFile marker;
  if (!Storage.openFileForRead("EBP", markerPath, marker)) {
    return false;
  }
  uint8_t version = 0;
  const bool valid = marker.read(&version, sizeof(version)) == sizeof(version) && marker.available() == 0 &&
                     version == FULL_CACHE_MARKER_VERSION;
  marker.close();
  return valid;
}

Epub::CacheGenerationStatus Epub::getCacheGenerationStatus() const {
  if (isFullCacheGenerated()) return CacheGenerationStatus::Complete;

  // A cache directory can be recreated solely to retain progress.bin after a
  // per-book cache deletion.  Only section files represent generated reading
  // cache that a later full-cache run can reuse.  Do not probe the parent
  // cache directory first: the section lookup already distinguishes a missing
  // cache from a resumable one, and this function runs once per visible book.
  return Storage.exists((cachePath + "/sections").c_str()) ? CacheGenerationStatus::Resumable
                                                           : CacheGenerationStatus::NotGenerated;
}

bool Epub::getSourceFingerprint(uint64_t* fingerprint) const {
  // TXT identity/legacy progress migration is handled separately from ZIP IDs.
  if (isPlainText()) return false;
  if (fingerprint == nullptr) return false;
  if (hasSourceFingerprint) {
    *fingerprint = cachedSourceFingerprint;
    return true;
  }

  ZipFile sourceZip(filepath);
  uint64_t computedFingerprint = 0;
  if (!sourceZip.getArchiveFingerprint(&computedFingerprint)) return false;

  cachedSourceFingerprint = computedFingerprint;
  hasSourceFingerprint = true;
  *fingerprint = computedFingerprint;
  return true;
}

void Epub::clearFullCacheGeneratedMarker() const {
  Storage.remove((cachePath + fullCacheMarkerTmpFile).c_str());
  Storage.remove((cachePath + fullCacheMarkerFile).c_str());
}

void Epub::clearCachePromptSeenMarker() const {
  Storage.remove((cachePath + cachePromptSeenMarkerFile).c_str());
  Storage.remove((cachePath + legacyNoCachePromptMarkerFile).c_str());
}

bool Epub::markFullCacheGenerated() const {
  const std::string markerPath = cachePath + fullCacheMarkerFile;
  const std::string tmpPath = cachePath + fullCacheMarkerTmpFile;
  Storage.remove(tmpPath.c_str());

  FsFile marker;
  if (!Storage.openFileForWrite("EBP", tmpPath, marker)) {
    LOG_ERR("EBP", "Could not create full-cache completion marker");
    return false;
  }
  const uint8_t version = FULL_CACHE_MARKER_VERSION;
  const bool written = marker.write(&version, sizeof(version)) == sizeof(version);
  marker.flush();
  marker.close();
  if (!written) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  Storage.remove(markerPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), markerPath.c_str())) {
    Storage.remove(tmpPath.c_str());
    LOG_ERR("EBP", "Could not publish full-cache completion marker");
    return false;
  }
  return true;
}

bool Epub::findContentOpfFile(std::string* contentOpfFile) const {
  const auto containerPath = "META-INF/container.xml";
  size_t containerSize;

  // Get file size without loading it all into heap
  if (!getItemSize(containerPath, &containerSize)) {
    LOG_ERR("EBP", "Could not find or size META-INF/container.xml");
    return false;
  }

  ContainerParser containerParser(containerSize);

  if (!containerParser.setup()) {
    return false;
  }

  // Stream read (reusing your existing stream logic)
  if (!readItemContentsToStream(containerPath, containerParser, 512)) {
    LOG_ERR("EBP", "Could not read META-INF/container.xml");
    return false;
  }

  // Extract the result
  if (containerParser.fullPath.empty()) {
    LOG_ERR("EBP", "Could not find valid rootfile in container.xml");
    return false;
  }

  *contentOpfFile = std::move(containerParser.fullPath);
  return true;
}

bool Epub::parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, const bool writeSpineEntries) {
  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath)) {
    LOG_ERR("EBP", "Could not find content.opf in zip");
    return false;
  }

  contentBasePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  LOG_DBG("EBP", "Parsing content.opf: %s", contentOpfFilePath.c_str());

  size_t contentOpfSize;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    LOG_ERR("EBP", "Could not get size of content.opf");
    return false;
  }

  ContentOpfParser opfParser(getCachePath(), getBasePath(), contentOpfSize,
                             writeSpineEntries ? bookMetadataCache.get() : nullptr);
  if (!opfParser.setup()) {
    LOG_ERR("EBP", "Could not setup content.opf parser");
    return false;
  }

  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024)) {
    LOG_ERR("EBP", "Could not read content.opf");
    return false;
  }

  // Grab data from opfParser into epub
  bookMetadata.title = opfParser.title;
  bookMetadata.author = opfParser.author;
  bookMetadata.language = opfParser.language;
  bookMetadata.coverItemHref = opfParser.coverItemHref;

  // Guide-based cover fallback: if no cover found via metadata/properties,
  // try extracting the image reference from the guide's cover page XHTML
  if (bookMetadata.coverItemHref.empty() && !opfParser.guideCoverPageHref.empty()) {
    LOG_DBG("EBP", "No cover from metadata, trying guide cover page: %s", opfParser.guideCoverPageHref.c_str());
    size_t coverPageSize;
    uint8_t* coverPageData = readItemContentsToBytes(opfParser.guideCoverPageHref, &coverPageSize, true);
    if (coverPageData) {
      const std::string coverPageHtml(reinterpret_cast<char*>(coverPageData), coverPageSize);
      free(coverPageData);

      // Determine base path of the cover page for resolving relative image references
      std::string coverPageBase;
      const auto lastSlash = opfParser.guideCoverPageHref.rfind('/');
      if (lastSlash != std::string::npos) {
        coverPageBase = opfParser.guideCoverPageHref.substr(0, lastSlash + 1);
      }

      // Search a guide cover page for a raster image nested in an SVG wrapper.
      std::string imageRef;
      findRasterCoverImageReference(coverPageHtml, imageRef);

      if (!imageRef.empty()) {
        bookMetadata.coverItemHref = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(coverPageBase + imageRef));
        LOG_DBG("EBP", "Found cover image from guide: %s", bookMetadata.coverItemHref.c_str());
      }
    }
  }

  bookMetadata.textReferenceHref = opfParser.textReferenceHref;
  bookMetadata.pageProgressionRtl = opfParser.pageProgressionRtl;

  if (!opfParser.tocNcxPath.empty()) {
    tocNcxItem = opfParser.tocNcxPath;
  }

  if (!opfParser.tocNavPath.empty()) {
    tocNavItem = opfParser.tocNavPath;
  }

  if (!opfParser.cssFiles.empty()) {
    cssFiles = opfParser.cssFiles;
  }

  LOG_DBG("EBP", "Successfully parsed content.opf");
  return true;
}

bool Epub::parseTocNcxFile() const {
  // the ncx file should have been specified in the content.opf file
  if (tocNcxItem.empty()) {
    LOG_DBG("EBP", "No ncx file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc ncx file: %s", tocNcxItem.c_str());

  // The parser can finalize a stream without knowing its size.  Parsing
  // directly avoids writing, rereading and deleting a full temporary NCX file
  // on the SD card during every metadata-cache rebuild.
  TocNcxParser ncxParser(contentBasePath, 0, bookMetadataCache.get());

  if (!ncxParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc ncx parser");
    return false;
  }

  if (!readItemContentsToStream(tocNcxItem, ncxParser, 1024) || !ncxParser.finish()) {
    LOG_ERR("EBP", "Could not parse toc ncx file");
    return false;
  }

  LOG_DBG("EBP", "Parsed TOC items");
  return true;
}

bool Epub::parseTocNavFile() const {
  // the nav file should have been specified in the content.opf file (EPUB 3)
  if (tocNavItem.empty()) {
    LOG_DBG("EBP", "No nav file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc nav file: %s", tocNavItem.c_str());

  // Note: We can't use `contentBasePath` here as the nav file may be in a different folder to the content.opf
  // and the HTMLX nav file will have hrefs relative to itself
  const std::string navContentBasePath = tocNavItem.substr(0, tocNavItem.find_last_of('/') + 1);
  // Avoid staging nav.xhtml on SD: the parser finalizes after the ZIP stream
  // ends, so its uncompressed size does not need to be measured first.
  TocNavParser navParser(navContentBasePath, 0, bookMetadataCache.get());

  if (!navParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc nav parser");
    return false;
  }

  if (!readItemContentsToStream(tocNavItem, navParser, 1024) || !navParser.finish()) {
    LOG_ERR("EBP", "Could not parse toc nav file");
    return false;
  }

  LOG_DBG("EBP", "Parsed TOC nav items");
  return true;
}

void Epub::discoverCssFilesFromZip() {
  const std::string& opfDir = contentBasePath;
  ZipFile zf(filepath);

  if (!zf.enumerateFilePaths([&](std::string_view filePath) {
        if (!opfDir.empty() && filePath.find(opfDir) != 0) {
          return;
        }

        if (!FsHelpers::hasCssExtension(filePath)) {
          return;
        }

        if (std::find(cssFiles.begin(), cssFiles.end(), filePath) != cssFiles.end()) {
          return;
        }

        LOG_DBG("EBP", "Discovered CSS file via ZIP enumeration: %.*s", (int)filePath.size(), filePath.data());
        cssFiles.push_back(std::string{filePath});
      })) {
    cssDiscoveryComplete = false;
    LOG_ERR("EBP", "Failed to enumerate ZIP file paths for CSS discovery");
  }
}

void Epub::parseCssFiles() const {
  // Maximum CSS file size we'll attempt to parse (uncompressed)
  // Larger files risk memory exhaustion on ESP32
  constexpr size_t MAX_CSS_FILE_SIZE = 128 * 1024;  // 128KB
  // Minimum heap required before attempting CSS parsing
  constexpr size_t MIN_HEAP_FOR_CSS_PARSING = 64 * 1024;  // 64KB

  if (cssFiles.empty()) {
    LOG_DBG("EBP", "No CSS files to parse, but CssParser created for inline styles");
  }

  LOG_DBG("EBP", "CSS files to parse: %zu", cssFiles.size());
  LOG_DBG("EBP", "CSS heap before parsing: free=%u, maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // Validate an existing cache before a book.bin rebuild reuses it.  Presence
  // alone is not enough: an interrupted write, a changed EPUB, or a truncated
  // file must fall through to a clean rebuild.
  if (cssParser->hasCache()) {
    if (cssParser->validateCache()) {
      LOG_DBG("EBP", "CSS cache validated, skipping parseCssFiles");
      return;
    }
    cssParser->deleteCache();
    LOG_DBG("EBP", "CSS cache invalid, rebuilding");
  }

  // Stream all supported rules to a temporary cache in source order. Publish
  // only after every stylesheet succeeds; partial output must never be reused.
  if (!cssParser->beginCacheWrite()) {
    LOG_ERR("EBP", "Could not start streaming CSS cache");
    return;
  }
  bool cssComplete = cssDiscoveryComplete;
  for (const auto& cssPath : cssFiles) {
    LOG_DBG("EBP", "Parsing CSS file: %s", cssPath.c_str());

    // Check heap before parsing - CSS parsing allocates heavily
    const uint32_t freeHeap = ESP.getFreeHeap();
#if defined(BATCH_CSS_MEMORY_DIAGNOSTICS)
    LOG_INF("BCMEM", "stage=css-gate free=%u maxAlloc=%u required=%u", freeHeap, ESP.getMaxAllocHeap(),
            static_cast<unsigned>(MIN_HEAP_FOR_CSS_PARSING));
#endif
    if (freeHeap < MIN_HEAP_FOR_CSS_PARSING) {
      LOG_ERR("EBP", "Insufficient heap for CSS parsing (%u bytes free, need %zu), skipping: %s", freeHeap,
              MIN_HEAP_FOR_CSS_PARSING, cssPath.c_str());
      cssComplete = false;
      continue;
    }

    // Check CSS file size before decompressing - skip files that are too large
    size_t cssFileSize = 0;
    if (getItemSize(cssPath, &cssFileSize)) {
      if (cssFileSize > MAX_CSS_FILE_SIZE) {
        LOG_ERR("EBP", "CSS file too large (%zu bytes > %zu max), skipping: %s", cssFileSize, MAX_CSS_FILE_SIZE,
                cssPath.c_str());
        cssComplete = false;
        continue;
      }
    }

    // Extract CSS file to temp location
    const auto tmpCssPath = getCachePath() + "/.tmp.css";
    FsFile tempCssFile;
    if (!Storage.openFileForWrite("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not create temp CSS file");
      cssComplete = false;
      continue;
    }
    if (!readItemContentsToStream(cssPath, tempCssFile, 1024)) {
      LOG_ERR("EBP", "Could not read CSS file: %s", cssPath.c_str());
      cssComplete = false;
      // Explicitly close() file before calling Storage.remove()
      tempCssFile.close();
      Storage.remove(tmpCssPath.c_str());
      continue;
    }
    // Explicitly close() file before reopening for reading
    tempCssFile.close();

    // Parse the CSS file
    if (!Storage.openFileForRead("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not open temp CSS file for reading");
      cssComplete = false;
      Storage.remove(tmpCssPath.c_str());
      continue;
    }
    if (!cssParser->loadFromStream(tempCssFile)) {
      cssComplete = false;
    }
    // Explicitly close() file before calling Storage.remove()
    tempCssFile.close();
    Storage.remove(tmpCssPath.c_str());
    LOG_DBG("EBP", "CSS heap after %s: rules=%zu, free=%u, maxAlloc=%u", cssPath.c_str(), cssParser->ruleCount(),
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }

  // A low-heap-truncated rule set must not become a permanent cache. A later
  // session with enough memory can then rebuild the complete stylesheet.
  if (!cssComplete) {
    cssParser->abortCacheWrite();
    LOG_ERR("EBP", "CSS parsing incomplete, not saving cache so it can be rebuilt later");
  } else if (!cssParser->finishCacheWrite()) {
    LOG_ERR("EBP", "Failed to save CSS rules to cache");
  }

  LOG_DBG("EBP", "CSS streaming pass finished for %zu files (complete=%d)", cssFiles.size(), cssComplete);
  cssParser->clear();
  LOG_DBG("EBP", "CSS rules released: free=%u, maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

// load in the meta data for the epub file
bool Epub::load(const bool buildIfMissing, const bool skipLoadingCss) {
  if (isPlainText()) return loadPlainText(buildIfMissing);
  cssDiscoveryComplete = true;
  LOG_DBG("EBP", "Loading ePub: %s", filepath.c_str());

  const uint32_t fingerprintStartedAt = millis();
  uint64_t sourceFingerprint = 0;
  const bool reusedSourceFingerprint = hasSourceFingerprint;
  if (!getSourceFingerprint(&sourceFingerprint)) {
    LOG_ERR("EBP", "Could not fingerprint EPUB source: %s", filepath.c_str());
    return false;
  }
  bool fingerprintNeedsWrite = false;
  if (!prepareSourceFingerprint(sourceFingerprint, fingerprintNeedsWrite)) {
    return false;
  }
  LOG_DBG("EBP", "EPUB source fingerprint %s in %lu ms", reusedSourceFingerprint ? "reused" : "checked",
          millis() - fingerprintStartedAt);

  // Initialize spine/TOC cache
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  // Always create CssParser - needed for inline style parsing even without CSS files
  cssParser.reset(new CssParser(cachePath));
  cssParser->setCacheSourceFingerprint(sourceFingerprint);

  // Try to load existing cache first
  if (bookMetadataCache->load()) {
    if (!skipLoadingCss) {
      // Keep the rules on SD until a section needs them. Loading the full map
      // merely to validate it can starve the section builder of heap.
      const bool cssCacheLoaded = cssParser->hasCache() && cssParser->validateCache();
      if (!cssCacheLoaded) {
        LOG_DBG("EBP", "CSS rules cache missing or stale, attempting to parse CSS files");
        cssParser->deleteCache();

        BookMetadataCache::BookMetadata cachedMetadata = bookMetadataCache->coreMetadata;
        const bool cssSourcesKnown = parseContentOpf(cachedMetadata, /*writeSpineEntries=*/false);
        if (!cssSourcesKnown) {
          LOG_ERR("EBP", "Could not parse content.opf from cached bookMetadata for CSS files");
          // continue anyway - book will work without CSS and we'll still load any inline style CSS
        } else {
          discoverCssFilesFromZip();
        }
        bookMetadataCache.reset();
        if (cssSourcesKnown) parseCssFiles();
        // parseCssFiles deliberately leaves no cache when low heap prevents a
        // complete stylesheet parse.  That is transient and must not discard
        // otherwise valid section caches on every book open.
        const bool cssCacheRebuilt = cssParser->hasCache();
        bookMetadataCache.reset(new BookMetadataCache(cachePath));
        if (!bookMetadataCache->load()) {
          LOG_ERR("EBP", "Failed to reload cache after CSS rebuild");
          return false;
        }
        if (cssCacheRebuilt) {
          // New CSS rules can affect layout, so rebuild section caches only
          // after a complete CSS cache was actually published.
          clearFullCacheGeneratedMarker();
          Storage.removeDir((cachePath + "/sections").c_str());
        } else {
          LOG_INF("EBP", "CSS cache was not rebuilt; retaining existing section caches");
        }
      } else {
        LOG_DBG("EBP", "Validated CSS cache without loading rules into RAM");
      }
    }
    if (fingerprintNeedsWrite && !saveSourceFingerprint(sourceFingerprint)) {
      LOG_ERR("EBP", "Loaded EPUB but could not persist its source fingerprint");
    }
    LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
    return true;
  }

  // If we didn't load from cache above and we aren't allowed to build, fail now
  if (!buildIfMissing) {
    return false;
  }

  // Cache doesn't exist or is invalid, build it
  LOG_DBG("EBP", "Cache not found, building spine/TOC cache");
  setupCacheDir();

  const uint32_t indexingStart = millis();

  // Begin building cache - stream entries to disk immediately
  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR("EBP", "Could not begin writing cache");
    return false;
  }

  // OPF Pass
  const uint32_t opfStart = millis();
  BookMetadataCache::BookMetadata bookMetadata;
  if (!bookMetadataCache->beginContentOpfPass()) {
    LOG_ERR("EBP", "Could not begin writing content.opf pass");
    return false;
  }
  if (!parseContentOpf(bookMetadata)) {
    LOG_ERR("EBP", "Could not parse content.opf");
    return false;
  }
  discoverCssFilesFromZip();
  if (!bookMetadataCache->endContentOpfPass()) {
    LOG_ERR("EBP", "Could not end writing content.opf pass");
    return false;
  }
  LOG_DBG("EBP", "OPF pass completed in %lu ms", millis() - opfStart);

  // TOC Pass - try EPUB 3 nav first, fall back to NCX
  const uint32_t tocStart = millis();
  if (!bookMetadataCache->beginTocPass()) {
    LOG_ERR("EBP", "Could not begin writing toc pass");
    return false;
  }

  bool tocParsed = false;

  // Try EPUB 3 nav document first (preferred)
  if (!tocNavItem.empty()) {
    LOG_DBG("EBP", "Attempting to parse EPUB 3 nav document");
    tocParsed = parseTocNavFile();
  }

  // Fall back to NCX if nav parsing failed or wasn't available
  if (!tocParsed && !tocNcxItem.empty()) {
    LOG_DBG("EBP", "Falling back to NCX TOC");
    tocParsed = parseTocNcxFile();
  }

  if (!tocParsed) {
    LOG_ERR("EBP", "Warning: Could not parse any TOC format");
    // Continue anyway - book will work without TOC
  }

  if (!bookMetadataCache->endTocPass()) {
    LOG_ERR("EBP", "Could not end writing toc pass");
    return false;
  }
  LOG_DBG("EBP", "TOC pass completed in %lu ms", millis() - tocStart);

  // Close the cache files
  if (!bookMetadataCache->endWrite()) {
    LOG_ERR("EBP", "Could not end writing cache");
    return false;
  }

  // Build final book.bin
  const uint32_t buildStart = millis();
  if (!bookMetadataCache->buildBookBin(filepath, bookMetadata)) {
    LOG_ERR("EBP", "Could not update mappings and sizes");
    return false;
  }
  LOG_DBG("EBP", "buildBookBin completed in %lu ms", millis() - buildStart);
  LOG_DBG("EBP", "Total indexing completed in %lu ms", millis() - indexingStart);

  if (!bookMetadataCache->cleanupTmpFiles()) {
    LOG_DBG("EBP", "Could not cleanup tmp files - ignoring");
  }

  if (!skipLoadingCss) {
    // Parse CSS before reloading book.bin to leave more heap for CSS rule-table growth.
    bookMetadataCache.reset();
    parseCssFiles();
    clearFullCacheGeneratedMarker();
    Storage.removeDir((cachePath + "/sections").c_str());
  }

  // Reload the cache from disk so it's in the correct state
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  if (!bookMetadataCache->load()) {
    LOG_ERR("EBP", "Failed to reload cache after writing");
    return false;
  }
  if (fingerprintNeedsWrite && !saveSourceFingerprint(sourceFingerprint)) {
    LOG_ERR("EBP", "Built EPUB cache but could not persist its source fingerprint");
  }
  LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
  return true;
}

bool Epub::removeCacheDirectory(const std::string& cachePath) {
  if (!Storage.exists(cachePath.c_str())) {
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("EPB", "Failed to remove cache directory: %s", cachePath.c_str());
    return false;
  }

  return true;
}

bool Epub::clearCache() const {
  if (!removeCacheDirectory(cachePath)) {
    LOG_ERR("EPB", "Failed to clear cache");
    return false;
  }

  LOG_DBG("EPB", "Cache cleared successfully");
  return true;
}

void Epub::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  Storage.mkdir(cachePath.c_str());
}

const std::string& Epub::getCachePath() const { return cachePath; }

const std::string& Epub::getPath() const { return filepath; }

const std::string& Epub::getTitle() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.title;
}

const std::string& Epub::getAuthor() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.author;
}

const std::string& Epub::getLanguage() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.language;
}

bool Epub::isPageProgressionRtl() const {
  return bookMetadataCache ? bookMetadataCache->coreMetadata.pageProgressionRtl : false;
}

std::string Epub::getCoverBmpPath(bool cropped) const {
  const auto coverFileName = std::string("cover") + (cropped ? "_crop" : "");
  return cachePath + "/" + coverFileName + ".bmp";
}

bool Epub::generateCoverBmp(bool cropped) const {
  // Already generated, return true
  if (Storage.exists(getCoverBmpPath(cropped).c_str())) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate cover BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = resolveSvgCoverImageHref(*this, bookMetadataCache->coreMetadata.coverItemHref);
  if (coverImageHref.empty()) {
    LOG_ERR("EBP", "No known cover image");
    return false;
  }

  if (FsHelpers::hasJpgExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating BMP from JPG cover image (%s mode)", cropped ? "cropped" : "fit");
    const auto coverJpgTempPath = getCachePath() + "/.cover.jpg";

    FsFile coverJpg;
    if (!Storage.openFileForWrite("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }
    readItemContentsToStream(coverImageHref, coverJpg, 1024);
    // Explicitly close() file before reopening for reading
    coverJpg.close();

    if (!Storage.openFileForRead("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }

    FsFile coverBmp;
    if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped), coverBmp)) {
      return false;
    }
    const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp, cropped);
    // Explicitly close() files before calling Storage.remove()
    coverJpg.close();
    coverBmp.close();
    Storage.remove(coverJpgTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate BMP from cover image");
      Storage.remove(getCoverBmpPath(cropped).c_str());
    }
    LOG_DBG("EBP", "Generated BMP from JPG cover image, success: %s", success ? "yes" : "no");
    return success;
  }

  if (FsHelpers::hasPngExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating BMP from PNG cover image (%s mode)", cropped ? "cropped" : "fit");
    const auto coverPngTempPath = getCachePath() + "/.cover.png";

    FsFile coverPng;
    if (!Storage.openFileForWrite("EBP", coverPngTempPath, coverPng)) {
      return false;
    }
    readItemContentsToStream(coverImageHref, coverPng, 1024);
    // Explicitly close() file before reopening for reading
    coverPng.close();

    if (!Storage.openFileForRead("EBP", coverPngTempPath, coverPng)) {
      return false;
    }

    FsFile coverBmp;
    if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped), coverBmp)) {
      return false;
    }
    const bool success = PngToBmpConverter::pngFileToBmpStream(coverPng, coverBmp, cropped);
    // Explicitly close() files before calling Storage.remove()
    coverPng.close();
    coverBmp.close();
    Storage.remove(coverPngTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate BMP from PNG cover image");
      Storage.remove(getCoverBmpPath(cropped).c_str());
    }
    LOG_DBG("EBP", "Generated BMP from PNG cover image, success: %s", success ? "yes" : "no");
    return success;
  }

  LOG_ERR("EBP", "Cover image is not a supported format, skipping");
  return false;
}

bool Epub::hasCoverImage() const {
  return bookMetadataCache && bookMetadataCache->isLoaded() && !bookMetadataCache->coreMetadata.coverItemHref.empty();
}

std::string Epub::getThumbBmpPath() const { return cachePath + "/thumb_[HEIGHT].bmp"; }
std::string Epub::getThumbBmpPath(int height) const { return cachePath + "/thumb_" + std::to_string(height) + ".bmp"; }

bool Epub::generateThumbBmp(int height) const {
  // Already generated, return true
  if (Storage.exists(getThumbBmpPath(height).c_str())) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate thumb BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = resolveSvgCoverImageHref(*this, bookMetadataCache->coreMetadata.coverItemHref);
  if (coverImageHref.empty()) {
    LOG_DBG("EBP", "No known cover image for thumbnail");
  } else if (FsHelpers::hasJpgExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating thumb BMP from JPG cover image");
    const auto coverJpgTempPath = getCachePath() + "/.cover.jpg";

    FsFile coverJpg;
    if (!Storage.openFileForWrite("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }
    readItemContentsToStream(coverImageHref, coverJpg, 1024);
    // Explicitly close() file before reopening for reading
    coverJpg.close();

    if (!Storage.openFileForRead("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }

    FsFile thumbBmp;
    if (!Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp)) {
      return false;
    }
    // Use smaller target size for Continue Reading card (half of screen: 240x400)
    // Generate 1-bit BMP for fast home screen rendering (no gray passes needed)
    int THUMB_TARGET_WIDTH = height * 0.6;
    int THUMB_TARGET_HEIGHT = height;
    const bool success = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(coverJpg, thumbBmp, THUMB_TARGET_WIDTH,
                                                                             THUMB_TARGET_HEIGHT);
    // Explicitly close() files before calling Storage.remove()
    coverJpg.close();
    thumbBmp.close();
    Storage.remove(coverJpgTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate thumb BMP from JPG cover image");
      Storage.remove(getThumbBmpPath(height).c_str());
    }
    LOG_DBG("EBP", "Generated thumb BMP from JPG cover image, success: %s", success ? "yes" : "no");
    return success;
  } else if (FsHelpers::hasPngExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating thumb BMP from PNG cover image");
    const auto coverPngTempPath = getCachePath() + "/.cover.png";

    FsFile coverPng;
    if (!Storage.openFileForWrite("EBP", coverPngTempPath, coverPng)) {
      return false;
    }
    readItemContentsToStream(coverImageHref, coverPng, 1024);
    // Explicitly close() file before reopening for reading
    coverPng.close();

    if (!Storage.openFileForRead("EBP", coverPngTempPath, coverPng)) {
      return false;
    }

    FsFile thumbBmp;
    if (!Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp)) {
      return false;
    }
    int THUMB_TARGET_WIDTH = height * 0.6;
    int THUMB_TARGET_HEIGHT = height;
    const bool success =
        PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(coverPng, thumbBmp, THUMB_TARGET_WIDTH, THUMB_TARGET_HEIGHT);
    // Explicitly close() files before calling Storage.remove()
    coverPng.close();
    thumbBmp.close();
    Storage.remove(coverPngTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate thumb BMP from PNG cover image");
      Storage.remove(getThumbBmpPath(height).c_str());
    }
    LOG_DBG("EBP", "Generated thumb BMP from PNG cover image, success: %s", success ? "yes" : "no");
    return success;
  } else {
    LOG_ERR("EBP", "Cover image is not a supported format, skipping thumbnail");
  }

  // Write an empty bmp file to avoid generation attempts in the future
  FsFile thumbBmp;
  Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp);
  return false;
}

uint8_t* Epub::readItemContentsToBytes(const std::string& itemHref, size_t* size, const bool trailingNullByte) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return nullptr;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);
  if (isPlainText()) {
    FsFile html;
    if (path != "content.html" || !Storage.openFileForRead("TXT", cachePath + "/text.xhtml", html)) return nullptr;
    const size_t length = html.size();
    if (length > SIZE_MAX - static_cast<size_t>(trailingNullByte)) return nullptr;
    auto* bytes = static_cast<uint8_t*>(malloc(length + static_cast<size_t>(trailingNullByte)));
    if (!bytes) return nullptr;
    if (html.read(bytes, length) != static_cast<int>(length)) {
      free(bytes);
      return nullptr;
    }
    if (trailingNullByte) bytes[length] = '\0';
    if (size) *size = length;
    return bytes;
  }

  const auto content = ZipFile(filepath).readFileToMemory(path.c_str(), size, trailingNullByte);
  if (!content) {
    LOG_DBG("EBP", "Failed to read item %s", path.c_str());
    return nullptr;
  }

  return content;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return false;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);
  if (isPlainText()) {
    return path == "content.html" &&
           Storage.readFileToStream((cachePath + "/text.xhtml").c_str(), out, chunkSize);
  }
  return ZipFile(filepath).readFileToStream(path.c_str(), out, chunkSize);
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  const std::string path = FsHelpers::normalisePath(itemHref);
  if (isPlainText()) {
    FsFile html;
    if (!size || path != "content.html" ||
        !Storage.openFileForRead("TXT", cachePath + "/text.xhtml", html)) return false;
    *size = html.size();
    return true;
  }
  return ZipFile(filepath).getInflatedFileSize(path.c_str(), size);
}

bool Epub::isPlainText() const { return Txt::isTextFile(filepath); }

bool Epub::loadPlainText(const bool buildIfMissing) {
  const HalStorage::StorageLock storageLock;
  bookMetadataCache.reset();
  cssParser.reset();
  Txt text(filepath, "/.crosspoint");
  if (!text.prepareHtmlCache(cachePath, buildIfMissing)) return false;
  bookMetadataCache.reset(new (std::nothrow) BookMetadataCache(cachePath));
  cssParser.reset(new (std::nothrow) CssParser(cachePath));
  if (!bookMetadataCache || !cssParser) return false;
  if (bookMetadataCache->load()) return true;
  if (!buildIfMissing) return false;

  size_t htmlSize = 0;
  if (!getItemSize("content.html", &htmlSize) || !bookMetadataCache->beginWrite() ||
      !bookMetadataCache->beginContentOpfPass()) return false;
  bookMetadataCache->createSpineEntry("content.html");
  if (!bookMetadataCache->endContentOpfPass() || !bookMetadataCache->beginTocPass()) return false;
  std::string title = text.getTitle();
  if (FsHelpers::hasMarkdownExtension(title)) title.resize(title.size() - 3);
  bookMetadataCache->createTocEntry(title, "content.html", "", 0);
  if (!bookMetadataCache->endTocPass() || !bookMetadataCache->endWrite()) return false;
  BookMetadataCache::BookMetadata metadata;
  metadata.title = title;
  metadata.language = "ja";
  if (!bookMetadataCache->buildBookBin(filepath, metadata, htmlSize)) return false;
  bookMetadataCache->cleanupTmpFiles();
  bookMetadataCache.reset(new (std::nothrow) BookMetadataCache(cachePath));
  return bookMetadataCache && bookMetadataCache->load();
}

int Epub::getSpineItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }
  return bookMetadataCache->getSpineCount();
}

size_t Epub::getCumulativeSpineItemSize(const int spineIndex) const { return getSpineItem(spineIndex).cumulativeSize; }

BookMetadataCache::SpineEntry Epub::getSpineItem(const int spineIndex) const {
  BookMetadataCache::SpineEntry entry;
  if (!tryGetSpineItem(spineIndex, entry)) {
    return {};
  }
  return entry;
}

bool Epub::tryGetSpineItem(const int spineIndex, BookMetadataCache::SpineEntry& entry) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineItem called but cache not loaded");
    return false;
  }

  if (spineIndex < 0 || spineIndex >= bookMetadataCache->getSpineCount()) {
    LOG_ERR("EBP", "getSpineItem index:%d is out of range", spineIndex);
    return false;
  }

  return bookMetadataCache->tryGetSpineEntry(spineIndex, entry);
}

BookMetadataCache::TocEntry Epub::getTocItem(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_DBG("EBP", "getTocItem called but cache not loaded");
    return {};
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_DBG("EBP", "getTocItem index:%d is out of range", tocIndex);
    return {};
  }

  return bookMetadataCache->getTocEntry(tocIndex);
}

int Epub::getTocItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }

  return bookMetadataCache->getTocCount();
}

// work out the section index for a toc index
int Epub::getSpineIndexForTocIndex(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex called but cache not loaded");
    return 0;
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex: tocIndex %d out of range", tocIndex);
    return 0;
  }

  const int spineIndex = bookMetadataCache->getTocEntry(tocIndex).spineIndex;
  if (spineIndex < 0) {
    LOG_DBG("EBP", "Section not found for TOC index %d", tocIndex);
    return 0;
  }

  return spineIndex;
}

int Epub::getTocIndexForSpineIndex(const int spineIndex) const {
  // The reader uses spineCount as the end-of-book position, not a chapter.
  if (spineIndex < 0 || spineIndex >= getSpineItemsCount()) return -1;
  return getSpineItem(spineIndex).tocIndex;
}

size_t Epub::getBookSize() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded() || bookMetadataCache->getSpineCount() == 0) {
    return 0;
  }
  return getCumulativeSpineItemSize(getSpineItemsCount() - 1);
}

int Epub::getSpineIndexForTextReference() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTextReference called but cache not loaded");
    return 0;
  }
  LOG_DBG("EBP", "Core Metadata: cover(%d)=%s, textReference(%d)=%s",
          bookMetadataCache->coreMetadata.coverItemHref.size(), bookMetadataCache->coreMetadata.coverItemHref.c_str(),
          bookMetadataCache->coreMetadata.textReferenceHref.size(),
          bookMetadataCache->coreMetadata.textReferenceHref.c_str());

  if (bookMetadataCache->coreMetadata.textReferenceHref.empty()) {
    // there was no textReference in epub, so we return 0 (the first chapter)
    return 0;
  }

  // loop through spine items to get the correct index matching the text href
  for (size_t i = 0; i < getSpineItemsCount(); i++) {
    if (getSpineItem(i).href == bookMetadataCache->coreMetadata.textReferenceHref) {
      LOG_DBG("EBP", "Text reference %s found at index %d", bookMetadataCache->coreMetadata.textReferenceHref.c_str(),
              i);
      return i;
    }
  }
  // This should not happen, as we checked for empty textReferenceHref earlier
  LOG_DBG("EBP", "Section not found for text reference");
  return 0;
}

// Calculate progress in book (returns 0.0-1.0)
float Epub::calculateProgress(const int currentSpineIndex, const float currentSpineRead) const {
  const size_t bookSize = getBookSize();
  if (bookSize == 0) {
    return 0.0f;
  }
  // Handle the reader's end-of-book position before looking up chapter sizes.
  if (currentSpineIndex >= getSpineItemsCount()) return 1.0f;
  if (currentSpineIndex < 0) return 0.0f;
  const size_t prevChapterSize = (currentSpineIndex >= 1) ? getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t curChapterSize = getCumulativeSpineItemSize(currentSpineIndex) - prevChapterSize;
  const float sectionProgSize = currentSpineRead * static_cast<float>(curChapterSize);
  const float totalProgress = static_cast<float>(prevChapterSize) + sectionProgSize;
  return totalProgress / static_cast<float>(bookSize);
}

int Epub::resolveHrefToSpineIndex(const std::string& href) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return -1;

  // Extract filename (remove #anchor)
  std::string target = href;
  size_t hashPos = target.find('#');
  if (hashPos != std::string::npos) target = target.substr(0, hashPos);

  // Same-file reference (anchor-only)
  if (target.empty()) return -1;

  // Extract just the filename for comparison
  size_t targetSlash = target.find_last_of('/');
  std::string targetFilename = (targetSlash != std::string::npos) ? target.substr(targetSlash + 1) : target;

  for (int i = 0; i < getSpineItemsCount(); i++) {
    const auto& spineHref = getSpineItem(i).href;
    // Try exact match first
    if (spineHref == target) return i;
    // Then filename-only match
    size_t spineSlash = spineHref.find_last_of('/');
    std::string spineFilename = (spineSlash != std::string::npos) ? spineHref.substr(spineSlash + 1) : spineHref;
    if (spineFilename == targetFilename) return i;
  }
  return -1;
}
