#include "BookMetadataCache.h"

#include <Logging.h>
#include <Serialization.h>
#include <ZipFile.h>

#include <algorithm>
#include <cstring>
#include <deque>

#include "FsHelpers.h"

namespace {
constexpr uint8_t BOOK_CACHE_VERSION = 7;
constexpr char bookBinFile[] = "/book.bin";
constexpr char tmpBookBinFile[] = "/book.bin.tmp";
constexpr char tmpSpineBinFile[] = "/spine.bin.tmp";
constexpr char tmpTocBinFile[] = "/toc.bin.tmp";
constexpr size_t MAX_METADATA_STRING_SIZE = 16 * 1024;
constexpr size_t LUT_VALIDATION_BATCH_SIZE = 64;
constexpr size_t SIZE_LOOKUP_CHUNK_SIZE = 2048;

template <typename T>
bool readPodChecked(FsFile& file, T& value) {
  return file.read(&value, sizeof(value)) == static_cast<int>(sizeof(value));
}

bool readMetadataString(FsFile& file, std::string& value, const size_t metadataEnd) {
  uint32_t length = 0;
  if (!readPodChecked(file, length) || length > MAX_METADATA_STRING_SIZE || file.position() > metadataEnd ||
      length > metadataEnd - file.position()) {
    return false;
  }

  value.resize(length);
  return length == 0 || file.read(&value[0], length) == static_cast<int>(length);
}

bool skipCacheString(FsFile& file, const size_t fileSize) {
  uint32_t length = 0;
  if (!readPodChecked(file, length) || file.position() > fileSize || length > fileSize - file.position()) {
    return false;
  }
  return file.seek(file.position() + length);
}

bool validateBookCache(FsFile& file, BookMetadataCache::BookMetadata& metadata, size_t& lutOffset, uint16_t& spineCount,
                       uint16_t& tocCount) {
  const size_t fileSize = file.size();
  constexpr size_t MIN_FILE_SIZE = sizeof(BOOK_CACHE_VERSION) + sizeof(uint32_t) + sizeof(spineCount) +
                                   sizeof(tocCount) + sizeof(uint32_t) * 5 + sizeof(bool);
  if (fileSize < MIN_FILE_SIZE || fileSize > UINT32_MAX || !file.seek(0)) {
    return false;
  }

  uint8_t version = 0;
  uint32_t storedLutOffset = 0;
  if (!readPodChecked(file, version) || version != BOOK_CACHE_VERSION || !readPodChecked(file, storedLutOffset) ||
      !readPodChecked(file, spineCount) || !readPodChecked(file, tocCount) || storedLutOffset > fileSize) {
    return false;
  }
  lutOffset = storedLutOffset;

  if (!readMetadataString(file, metadata.title, lutOffset) || !readMetadataString(file, metadata.author, lutOffset) ||
      !readMetadataString(file, metadata.language, lutOffset) ||
      !readMetadataString(file, metadata.coverItemHref, lutOffset) ||
      !readMetadataString(file, metadata.textReferenceHref, lutOffset) ||
      !readPodChecked(file, metadata.pageProgressionRtl) || file.position() != lutOffset) {
    return false;
  }

  const uint32_t entryCount = static_cast<uint32_t>(spineCount) + tocCount;
  const uint32_t lutSize = entryCount * sizeof(uint32_t);
  if (lutOffset > fileSize || lutSize > fileSize - lutOffset) {
    return false;
  }

  const size_t dataStart = lutOffset + lutSize;
  size_t lutReadPos = lutOffset;
  size_t dataPos = dataStart;
  uint32_t entryIndex = 0;
  uint32_t offsets[LUT_VALIDATION_BATCH_SIZE];

  while (entryIndex < entryCount) {
    const size_t batchCount = std::min<size_t>(entryCount - entryIndex, LUT_VALIDATION_BATCH_SIZE);
    if (!file.seek(lutReadPos) ||
        file.read(offsets, batchCount * sizeof(uint32_t)) != static_cast<int>(batchCount * sizeof(uint32_t)) ||
        !file.seek(dataPos)) {
      return false;
    }

    for (size_t i = 0; i < batchCount; ++i, ++entryIndex) {
      if (offsets[i] != file.position() || offsets[i] < dataStart || offsets[i] >= fileSize) {
        return false;
      }

      if (entryIndex < spineCount) {
        uint32_t cumulativeSize = 0;
        int16_t tocIndex = -1;
        if (!skipCacheString(file, fileSize) || !readPodChecked(file, cumulativeSize) ||
            !readPodChecked(file, tocIndex) || tocIndex < -1 ||
            (tocIndex >= 0 && static_cast<uint16_t>(tocIndex) >= tocCount)) {
          return false;
        }
      } else {
        uint8_t level = 0;
        int16_t spineIndex = -1;
        if (!skipCacheString(file, fileSize) || !skipCacheString(file, fileSize) || !skipCacheString(file, fileSize) ||
            !readPodChecked(file, level) || !readPodChecked(file, spineIndex) || spineIndex < -1 ||
            (spineIndex >= 0 && static_cast<uint16_t>(spineIndex) >= spineCount)) {
          return false;
        }
      }
    }

    dataPos = file.position();
    lutReadPos += batchCount * sizeof(uint32_t);
  }

  return dataPos == fileSize;
}

// FsFile writes are expensive on the SD card when entries are serialized in
// several small fields. Buffer final book.bin output without changing its byte
// layout or requiring heap allocation.
class BufferedFileWriter {
  static constexpr size_t BUFFER_SIZE = 512;

  FsFile& file;
  uint8_t buffer[BUFFER_SIZE] = {};
  size_t used = 0;

 public:
  explicit BufferedFileWriter(FsFile& file) : file(file) {}

  uint32_t position() const { return file.position() + used; }

  void write(const uint8_t* data, size_t size) {
    while (size > 0) {
      if (used == BUFFER_SIZE) {
        flush();
      }

      if (used == 0 && size >= BUFFER_SIZE) {
        file.write(data, size);
        return;
      }

      const size_t chunkSize = std::min(size, BUFFER_SIZE - used);
      memcpy(buffer + used, data, chunkSize);
      used += chunkSize;
      data += chunkSize;
      size -= chunkSize;
    }
  }

  template <typename T>
  void writePod(const T& value) {
    write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
  }

  void writeString(const std::string& value) {
    const uint32_t length = value.size();
    writePod(length);
    write(reinterpret_cast<const uint8_t*>(value.data()), length);
  }

  void flush() {
    if (used == 0) {
      return;
    }
    file.write(buffer, used);
    used = 0;
  }
};
}  // namespace

/* ============= WRITING / BUILDING FUNCTIONS ================ */

bool BookMetadataCache::beginWrite() {
  buildMode = true;
  spineCount = 0;
  tocCount = 0;
  LOG_DBG("BMC", "Entering write mode");
  return true;
}

bool BookMetadataCache::beginContentOpfPass() {
  LOG_DBG("BMC", "Beginning content opf pass");

  // Open spine file for writing
  return Storage.openFileForWrite("BMC", cachePath + tmpSpineBinFile, spineFile);
}

bool BookMetadataCache::endContentOpfPass() {
  // Explicit close() required: member variable persists beyond function scope
  spineFile.close();
  return true;
}

bool BookMetadataCache::beginTocPass() {
  LOG_DBG("BMC", "Beginning toc pass");

  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    return false;
  }
  if (!Storage.openFileForWrite("BMC", cachePath + tmpTocBinFile, tocFile)) {
    // Explicit close() required: member variable persists beyond function scope
    spineFile.close();
    return false;
  }

  if (spineCount >= FAST_INDEX_THRESHOLD) {
    spineHrefIndex.clear();
    spineHrefIndex.resize(spineCount);
    spineFile.seek(0);
    for (int i = 0; i < spineCount; i++) {
      auto entry = readSpineEntry(spineFile);
      SpineHrefIndexEntry idx;
      idx.hrefHash = fnvHash64(entry.href);
      idx.hrefLen = static_cast<uint16_t>(entry.href.size());
      idx.spineIndex = static_cast<int16_t>(i);
      spineHrefIndex[i] = idx;
    }
    std::sort(spineHrefIndex.begin(), spineHrefIndex.end(),
              [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
              });
    spineFile.seek(0);
    useSpineHrefIndex = true;
    LOG_DBG("BMC", "Using fast index for %d spine items", spineCount);
  } else {
    useSpineHrefIndex = false;
  }

  return true;
}

bool BookMetadataCache::endTocPass() {
  // Explicit close() required: member variables persist beyond function scope
  tocFile.close();
  spineFile.close();

  spineHrefIndex.clear();
  spineHrefIndex.shrink_to_fit();
  useSpineHrefIndex = false;

  return true;
}

bool BookMetadataCache::endWrite() {
  if (!buildMode) {
    LOG_DBG("BMC", "endWrite called but not in build mode");
    return false;
  }

  buildMode = false;
  LOG_DBG("BMC", "Wrote %d spine, %d TOC entries", spineCount, tocCount);
  return true;
}

bool BookMetadataCache::buildBookBin(const std::string& epubPath, const BookMetadata& metadata) {
  const uint32_t buildStartedAt = millis();
  const std::string finalBookPath = cachePath + bookBinFile;
  const std::string tmpBookPath = cachePath + tmpBookBinFile;
  Storage.remove(tmpBookPath.c_str());
  // Open all three files, writing to meta, reading from spine and toc
  if (!Storage.openFileForWrite("BMC", tmpBookPath, bookFile)) {
    return false;
  }

  const auto discardTmpBook = [&]() {
    bookFile.close();
    spineFile.close();
    tocFile.close();
    Storage.remove(tmpBookPath.c_str());
  };

  if (!Storage.openFileForRead("BMC", cachePath + tmpSpineBinFile, spineFile)) {
    discardTmpBook();
    return false;
  }

  if (!Storage.openFileForRead("BMC", cachePath + tmpTocBinFile, tocFile)) {
    discardTmpBook();
    return false;
  }

  constexpr uint32_t headerASize =
      sizeof(BOOK_CACHE_VERSION) + /* LUT Offset */ sizeof(uint32_t) + sizeof(spineCount) + sizeof(tocCount);
  const uint32_t metadataSize = metadata.title.size() + metadata.author.size() + metadata.language.size() +
                                metadata.coverItemHref.size() + metadata.textReferenceHref.size() +
                                sizeof(uint32_t) * 5 + sizeof(bool);
  const uint32_t lutSize = sizeof(uint32_t) * spineCount + sizeof(uint32_t) * tocCount;
  const uint32_t lutOffset = headerASize + metadataSize;

  const uint32_t headerAndLutStartedAt = millis();
  // Header A
  serialization::writePod(bookFile, BOOK_CACHE_VERSION);
  serialization::writePod(bookFile, lutOffset);
  serialization::writePod(bookFile, spineCount);
  serialization::writePod(bookFile, tocCount);
  // Metadata
  serialization::writeString(bookFile, metadata.title);
  serialization::writeString(bookFile, metadata.author);
  serialization::writeString(bookFile, metadata.language);
  serialization::writeString(bookFile, metadata.coverItemHref);
  serialization::writeString(bookFile, metadata.textReferenceHref);
  serialization::writePod(bookFile, metadata.pageProgressionRtl);

  // Reserve the lookup-table region.  The final spine and TOC positions are
  // known only after their entries are written, so collect and backfill them
  // below. This avoids a complete extra read of both temporary files.
  constexpr size_t LUT_ZERO_CHUNK_SIZE = 64;
  const uint8_t zeroLutChunk[LUT_ZERO_CHUNK_SIZE] = {};
  for (uint32_t remaining = lutSize; remaining > 0;) {
    const size_t chunkSize = std::min<size_t>(remaining, LUT_ZERO_CHUNK_SIZE);
    bookFile.write(zeroLutChunk, chunkSize);
    remaining -= chunkSize;
  }
  LOG_DBG("BMC", "book.bin header/LUT reservation: %lu ms", millis() - headerAndLutStartedAt);

  std::deque<uint32_t> spineLutPositions(spineCount);
  std::deque<uint32_t> tocLutPositions(tocCount);
  BufferedFileWriter finalWriter(bookFile);

  // Loop through spines from spine file, calculate cumulative sizes, and
  // record each final book.bin position for the LUT.

  // Build spineIndex->tocIndex mapping in one pass (O(n) instead of O(n*m))
  const uint32_t tocMappingStartedAt = millis();
  std::deque<int16_t> spineToTocIndex(spineCount, -1);
  tocFile.seek(0);
  for (int j = 0; j < tocCount; j++) {
    auto tocEntry = readTocEntry(tocFile);
    if (tocEntry.spineIndex >= 0 && tocEntry.spineIndex < spineCount) {
      if (spineToTocIndex[tocEntry.spineIndex] == -1) {
        spineToTocIndex[tocEntry.spineIndex] = static_cast<int16_t>(j);
      }
    }
  }
  LOG_DBG("BMC", "spine-to-TOC mapping: %lu ms", millis() - tocMappingStartedAt);

  const uint32_t zipOpenStartedAt = millis();
  ZipFile zip(epubPath);
  // Pre-open zip file to speed up size calculations
  if (!zip.open()) {
    LOG_ERR("BMC", "Could not open EPUB zip for size calculations");
    discardTmpBook();
    return false;
  }
  LOG_DBG("BMC", "ZIP open: %lu ms", millis() - zipOpenStartedAt);
  // NOTE: We intentionally skip calling loadAllFileStatSlims() here.
  // For large EPUBs (2000+ chapters), pre-loading all ZIP central directory entries
  // into memory causes OOM crashes on ESP32-C3's limited ~380KB RAM.
  // Instead, for large books we match spine targets in bounded chunks against
  // the ZIP central directory, avoiding an allocation for every chapter at once.
  // See: https://github.com/crosspoint-reader/crosspoint-reader/issues/134

  std::deque<uint32_t> spineSizes;
  bool useBatchSizes = false;

  if (spineCount >= FAST_INDEX_THRESHOLD) {
    LOG_DBG("BMC", "Using batch size lookup for %d spine items", spineCount);

    std::deque<ZipFile::SizeTarget> targets;
    spineSizes.resize(spineCount, 0);
    int matched = 0;

    spineFile.seek(0);
    for (int i = 0; i < spineCount; i++) {
      auto entry = readSpineEntry(spineFile);
      std::string path = FsHelpers::normalisePath(entry.href);

      ZipFile::SizeTarget t;
      t.hash = ZipFile::fnvHash64(path.c_str(), path.size());
      t.len = static_cast<uint16_t>(path.size());
      t.index = static_cast<uint16_t>(i);
      targets.push_back(t);
      if (targets.size() < SIZE_LOOKUP_CHUNK_SIZE && i + 1 < spineCount) continue;

      std::sort(targets.begin(), targets.end(), [](const ZipFile::SizeTarget& a, const ZipFile::SizeTarget& b) {
        return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
      });
      matched += zip.fillUncompressedSizes(targets, spineSizes);
      targets.clear();
    }

    LOG_DBG("BMC", "Batch lookup matched %d/%d spine items", matched, spineCount);

    targets.shrink_to_fit();

    useBatchSizes = true;
  }

  uint32_t cumSize = 0;
  const uint32_t spineWriteStartedAt = millis();
  spineFile.seek(0);
  int lastSpineTocIndex = -1;
  for (int i = 0; i < spineCount; i++) {
    auto spineEntry = readSpineEntry(spineFile);

    spineEntry.tocIndex = spineToTocIndex[i];

    // Not a huge deal if we don't fine a TOC entry for the spine entry, this is expected behaviour for EPUBs
    // Logging here is for debugging
    if (spineEntry.tocIndex == -1) {
      LOG_DBG("BMC", "Warning: Could not find TOC entry for spine item %d: %s, using title from last section", i,
              spineEntry.href.c_str());
      spineEntry.tocIndex = lastSpineTocIndex;
    }
    lastSpineTocIndex = spineEntry.tocIndex;

    size_t itemSize = 0;
    if (useBatchSizes) {
      itemSize = spineSizes[i];
      if (itemSize == 0) {
        const std::string path = FsHelpers::normalisePath(spineEntry.href);
        if (!zip.getInflatedFileSize(path.c_str(), &itemSize)) {
          LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", path.c_str());
        }
      }
    } else {
      const std::string path = FsHelpers::normalisePath(spineEntry.href);
      if (!zip.getInflatedFileSize(path.c_str(), &itemSize)) {
        LOG_ERR("BMC", "Warning: Could not get size for spine item: %s", path.c_str());
      }
    }

    cumSize += itemSize;
    spineEntry.cumulativeSize = cumSize;

    spineLutPositions[i] = finalWriter.position();
    finalWriter.writeString(spineEntry.href);
    // The on-disk cache format uses 32-bit offsets on ESP32. Keep the same
    // layout in the 64-bit desktop simulator, where size_t is wider.
    finalWriter.writePod(static_cast<uint32_t>(spineEntry.cumulativeSize));
    finalWriter.writePod(spineEntry.tocIndex);
  }
  finalWriter.flush();
  LOG_DBG("BMC", "spine size lookup/write: %lu ms", millis() - spineWriteStartedAt);
  // Close opened zip file
  zip.close();

  // Loop through toc entries from toc file, recording final positions for the LUT.
  const uint32_t tocWriteStartedAt = millis();
  tocFile.seek(0);
  for (int i = 0; i < tocCount; i++) {
    auto tocEntry = readTocEntry(tocFile);
    tocLutPositions[i] = finalWriter.position();
    finalWriter.writeString(tocEntry.title);
    finalWriter.writeString(tocEntry.href);
    finalWriter.writeString(tocEntry.anchor);
    finalWriter.writePod(tocEntry.level);
    finalWriter.writePod(tocEntry.spineIndex);
  }
  finalWriter.flush();
  LOG_DBG("BMC", "TOC write: %lu ms", millis() - tocWriteStartedAt);

  const uint32_t lutBackfillStartedAt = millis();
  bookFile.seek(lutOffset);
  for (const uint32_t pos : spineLutPositions) {
    serialization::writePod(bookFile, pos);
  }
  for (const uint32_t pos : tocLutPositions) {
    serialization::writePod(bookFile, pos);
  }
  LOG_DBG("BMC", "book.bin LUT backfill: %lu ms", millis() - lutBackfillStartedAt);

  // Explicit close() required: member variables persist beyond function scope
  bookFile.close();
  spineFile.close();
  tocFile.close();

  FsFile validationFile;
  BookMetadata validatedMetadata;
  size_t validatedLutOffset = 0;
  uint16_t validatedSpineCount = 0;
  uint16_t validatedTocCount = 0;
  if (!Storage.openFileForRead("BMC", tmpBookPath, validationFile) ||
      !validateBookCache(validationFile, validatedMetadata, validatedLutOffset, validatedSpineCount,
                         validatedTocCount)) {
    validationFile.close();
    Storage.remove(tmpBookPath.c_str());
    LOG_ERR("BMC", "Generated book.bin failed validation");
    return false;
  }
  validationFile.close();

  if (Storage.exists(finalBookPath.c_str()) && !Storage.remove(finalBookPath.c_str())) {
    Storage.remove(tmpBookPath.c_str());
    LOG_ERR("BMC", "Could not remove old book.bin before replacement");
    return false;
  }
  if (!Storage.rename(tmpBookPath.c_str(), finalBookPath.c_str())) {
    Storage.remove(tmpBookPath.c_str());
    LOG_ERR("BMC", "Could not publish validated book.bin");
    return false;
  }

  LOG_DBG("BMC", "Successfully built book.bin in %lu ms", millis() - buildStartedAt);
  return true;
}

bool BookMetadataCache::cleanupTmpFiles() const {
  const auto spineBinFile = cachePath + tmpSpineBinFile;
  if (Storage.exists(spineBinFile.c_str())) {
    Storage.remove(spineBinFile.c_str());
  }
  const auto tocBinFile = cachePath + tmpTocBinFile;
  if (Storage.exists(tocBinFile.c_str())) {
    Storage.remove(tocBinFile.c_str());
  }
  const auto bookBinTmpFile = cachePath + tmpBookBinFile;
  if (Storage.exists(bookBinTmpFile.c_str())) {
    Storage.remove(bookBinTmpFile.c_str());
  }
  return true;
}

uint32_t BookMetadataCache::writeSpineEntry(FsFile& file, const SpineEntry& entry) const {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.href);
  serialization::writePod(file, entry.cumulativeSize);
  serialization::writePod(file, entry.tocIndex);
  return pos;
}

uint32_t BookMetadataCache::writeTocEntry(FsFile& file, const TocEntry& entry) const {
  const uint32_t pos = file.position();
  serialization::writeString(file, entry.title);
  serialization::writeString(file, entry.href);
  serialization::writeString(file, entry.anchor);
  serialization::writePod(file, entry.level);
  serialization::writePod(file, entry.spineIndex);
  return pos;
}

// Note: for the LUT to be accurate, this **MUST** be called for all spine items before `addTocEntry` is ever called
// this is because in this function we're marking positions of the items
void BookMetadataCache::createSpineEntry(const std::string& href) {
  if (!buildMode || !spineFile) {
    LOG_DBG("BMC", "createSpineEntry called but not in build mode");
    return;
  }

  const SpineEntry entry(href, 0, -1);
  writeSpineEntry(spineFile, entry);
  spineCount++;
}

void BookMetadataCache::createTocEntry(const std::string& title, const std::string& href, const std::string& anchor,
                                       const uint8_t level) {
  if (!buildMode || !tocFile || !spineFile) {
    LOG_DBG("BMC", "createTocEntry called but not in build mode");
    return;
  }

  int16_t spineIndex = -1;

  if (useSpineHrefIndex) {
    uint64_t targetHash = fnvHash64(href);
    uint16_t targetLen = static_cast<uint16_t>(href.size());

    auto it =
        std::lower_bound(spineHrefIndex.begin(), spineHrefIndex.end(), SpineHrefIndexEntry{targetHash, targetLen, 0},
                         [](const SpineHrefIndexEntry& a, const SpineHrefIndexEntry& b) {
                           return a.hrefHash < b.hrefHash || (a.hrefHash == b.hrefHash && a.hrefLen < b.hrefLen);
                         });

    while (it != spineHrefIndex.end() && it->hrefHash == targetHash && it->hrefLen == targetLen) {
      spineIndex = it->spineIndex;
      break;
    }

    if (spineIndex == -1) {
      LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
    }
  } else {
    spineFile.seek(0);
    for (int i = 0; i < spineCount; i++) {
      auto spineEntry = readSpineEntry(spineFile);
      if (spineEntry.href == href) {
        spineIndex = static_cast<int16_t>(i);
        break;
      }
    }
    if (spineIndex == -1) {
      LOG_DBG("BMC", "createTocEntry: Could not find spine item for TOC href %s", href.c_str());
    }
  }

  const TocEntry entry(title, href, anchor, level, spineIndex);
  writeTocEntry(tocFile, entry);
  tocCount++;
}

/* ============= READING / LOADING FUNCTIONS ================ */

bool BookMetadataCache::load() {
  if (!Storage.openFileForRead("BMC", cachePath + bookBinFile, bookFile)) {
    return false;
  }

  BookMetadata metadata;
  size_t validatedLutOffset = 0;
  uint16_t validatedSpineCount = 0;
  uint16_t validatedTocCount = 0;
  if (!validateBookCache(bookFile, metadata, validatedLutOffset, validatedSpineCount, validatedTocCount)) {
    LOG_ERR("BMC", "book.bin validation failed; rebuilding cache");
    bookFile.close();
    return false;
  }

  coreMetadata = std::move(metadata);
  lutOffset = validatedLutOffset;
  spineCount = validatedSpineCount;
  tocCount = validatedTocCount;
  loaded = true;
  LOG_DBG("BMC", "Loaded cache data: %d spine, %d TOC entries", spineCount, tocCount);
  return true;
}

BookMetadataCache::SpineEntry BookMetadataCache::getSpineEntry(const int index) {
  SpineEntry entry;
  if (!tryGetSpineEntry(index, entry)) {
    return {};
  }
  return entry;
}

bool BookMetadataCache::tryGetSpineEntry(const int index, SpineEntry& entry) {
  if (!loaded) {
    LOG_ERR("BMC", "getSpineEntry called but cache not loaded");
    return false;
  }

  if (index < 0 || index >= static_cast<int>(spineCount)) {
    LOG_ERR("BMC", "getSpineEntry index %d out of range", index);
    return false;
  }

  // The SD card can disappear after book.bin was validated and opened. Check
  // every read before trusting its length; an uninitialized string length here
  // previously reached std::string::resize() and aborted the device.
  const size_t fileSize = bookFile.size();
  const size_t lutEntryPos = lutOffset + sizeof(uint32_t) * static_cast<size_t>(index);
  uint32_t spineEntryPos = 0;
  uint32_t cumulativeSize = 0;
  SpineEntry candidate;
  if (fileSize == 0 || lutEntryPos > fileSize || sizeof(spineEntryPos) > fileSize - lutEntryPos ||
      !bookFile.seek(lutEntryPos) || !readPodChecked(bookFile, spineEntryPos) || spineEntryPos >= fileSize ||
      !bookFile.seek(spineEntryPos) || !readMetadataString(bookFile, candidate.href, fileSize) ||
      !readPodChecked(bookFile, cumulativeSize) || !readPodChecked(bookFile, candidate.tocIndex)) {
    LOG_ERR("BMC", "Failed to read spine entry %d", index);
    return false;
  }

  candidate.cumulativeSize = cumulativeSize;
  entry = std::move(candidate);
  return true;
}

BookMetadataCache::TocEntry BookMetadataCache::getTocEntry(const int index) {
  if (!loaded) {
    LOG_ERR("BMC", "getTocEntry called but cache not loaded");
    return {};
  }

  if (index < 0 || index >= static_cast<int>(tocCount)) {
    LOG_ERR("BMC", "getTocEntry index %d out of range", index);
    return {};
  }

  // Seek to TOC LUT item, read from LUT and get out data
  bookFile.seek(lutOffset + sizeof(uint32_t) * spineCount + sizeof(uint32_t) * index);
  uint32_t tocEntryPos;
  serialization::readPod(bookFile, tocEntryPos);
  bookFile.seek(tocEntryPos);
  return readTocEntry(bookFile);
}

BookMetadataCache::SpineEntry BookMetadataCache::readSpineEntry(FsFile& file) const {
  SpineEntry entry;
  serialization::readString(file, entry.href);
  serialization::readPod(file, entry.cumulativeSize);
  serialization::readPod(file, entry.tocIndex);
  return entry;
}

BookMetadataCache::TocEntry BookMetadataCache::readTocEntry(FsFile& file) const {
  TocEntry entry;
  serialization::readString(file, entry.title);
  serialization::readString(file, entry.href);
  serialization::readString(file, entry.anchor);
  serialization::readPod(file, entry.level);
  serialization::readPod(file, entry.spineIndex);
  return entry;
}
