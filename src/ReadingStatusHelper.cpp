#include "ReadingStatusHelper.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include "Epub.h"
#include "BookIdentity.h"
#include "ReadingHistoryStore.h"
#include "activities/reader/ProgressFile.h"
#include "util/BookDataPath.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

constexpr char BOOK_LIST_STATUS_INDEX_FILE[] = "/book-list-status.bin";
constexpr char BOOK_LIST_STATUS_INDEX_TMP_FILE[] = "/book-list-status.tmp";
constexpr uint32_t BOOK_LIST_STATUS_INDEX_MAGIC = 0x31534C42;  // "BLS1"
constexpr uint8_t BOOK_LIST_STATUS_INDEX_VERSION = 1;
constexpr uint16_t BOOK_LIST_STATUS_INDEX_MAX_ENTRIES = 2048;
constexpr uint8_t BOOK_LIST_STATUS_INDEX_MAX_NAME_LENGTH = 63;

bool getCacheEntryName(const std::string& filepath, std::string& entryName, bool& isEpub) {
  const char* prefix;
  if (FsHelpers::hasEpubExtension(filepath)) {
    prefix = "epub_";
    isEpub = true;
  } else if (FsHelpers::hasXtcExtension(filepath)) {
    prefix = "xtc_";
    isEpub = false;
  } else if (FsHelpers::hasTxtExtension(filepath) || FsHelpers::hasMarkdownExtension(filepath)) {
    prefix = "txt_";
    isEpub = false;
  } else {
    return false;
  }

  entryName = prefix + std::to_string(std::hash<std::string>{}(filepath));
  return true;
}

ReadingStatus readProgress(const std::string& progressPath, bool isEpub) {
  if (!Storage.exists(progressPath.c_str())) return ReadingStatus::Unread;

  FsFile file;
  if (!Storage.openFileForRead("RSH", progressPath, file)) {
    return ReadingStatus::Unread;
  }

  uint8_t data[7];
  const int bytesRead = file.read(data, sizeof(data));
  file.close();

  if (bytesRead <= 0) {
    return ReadingStatus::Unread;
  }

  const int flagOffset = isEpub ? 6 : 4;
  return bytesRead > flagOffset && data[flagOffset] == 1 ? ReadingStatus::Finished : ReadingStatus::Reading;
}

bool isValidReadingStatus(const uint8_t value) { return value <= static_cast<uint8_t>(ReadingStatus::Finished); }

bool isValidCachedBookStatus(const uint8_t value) {
  return value <= static_cast<uint8_t>(CachedBookStatus::Complete) || value == static_cast<uint8_t>(CachedBookStatus::Unknown);
}

bool cacheEntryExists(const std::vector<std::string>& cacheEntries, const std::string& cacheEntryName) {
  return std::binary_search(cacheEntries.begin(), cacheEntries.end(), cacheEntryName);
}

}  // namespace

void loadBookListStatusIndex(const std::string& cacheDir, std::vector<BookListStatusEntry>& entries) {
  entries.clear();
  const std::string indexPath = cacheDir + BOOK_LIST_STATUS_INDEX_FILE;
  if (!Storage.exists(indexPath.c_str())) return;
  FsFile file;
  if (!Storage.openFileForRead("BLI", indexPath, file)) return;

  uint32_t magic = 0;
  uint8_t version = 0;
  uint16_t count = 0;
  const bool validHeader = file.read(&magic, sizeof(magic)) == sizeof(magic) &&
                           file.read(&version, sizeof(version)) == sizeof(version) &&
                           file.read(&count, sizeof(count)) == sizeof(count) && magic == BOOK_LIST_STATUS_INDEX_MAGIC &&
                           version == BOOK_LIST_STATUS_INDEX_VERSION && count <= BOOK_LIST_STATUS_INDEX_MAX_ENTRIES;
  if (!validHeader) {
    file.close();
    LOG_DBG("BLI", "Ignoring invalid book-list status index");
    return;
  }

  entries.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    uint8_t nameLength = 0;
    if (file.read(&nameLength, sizeof(nameLength)) != sizeof(nameLength) || nameLength == 0 ||
        nameLength > BOOK_LIST_STATUS_INDEX_MAX_NAME_LENGTH) {
      entries.clear();
      break;
    }
    std::string cacheEntryName(nameLength, '\0');
    uint8_t reading = 0;
    uint8_t cache = 0;
    if (file.read(cacheEntryName.data(), nameLength) != nameLength || file.read(&reading, sizeof(reading)) != sizeof(reading) ||
        file.read(&cache, sizeof(cache)) != sizeof(cache) || !isValidReadingStatus(reading) ||
        !isValidCachedBookStatus(cache)) {
      entries.clear();
      break;
    }
    entries.push_back({std::move(cacheEntryName), static_cast<ReadingStatus>(reading), static_cast<CachedBookStatus>(cache)});
  }
  file.close();
  if (!std::is_sorted(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.cacheEntryName < b.cacheEntryName;
      })) {
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.cacheEntryName < b.cacheEntryName; });
  }
  LOG_DBG("BLI", "Loaded book-list status index: entries=%lu", static_cast<unsigned long>(entries.size()));
}

bool getBookListStatusFromIndex(const std::string& filepath, const std::vector<std::string>& cacheEntries,
                                const std::vector<BookListStatusEntry>& entries, ReadingStatus& readingStatus,
                                CachedBookStatus& cacheStatus) {
  std::string cacheEntryName;
  bool isEpub = false;
  if (!getCacheEntryName(filepath, cacheEntryName, isEpub)) return false;
  const auto entry = std::lower_bound(entries.begin(), entries.end(), cacheEntryName,
                                      [](const BookListStatusEntry& value, const std::string& name) {
                                        return value.cacheEntryName < name;
                                      });
  if (entry == entries.end() || entry->cacheEntryName != cacheEntryName) return false;
  readingStatus = entry->readingStatus;
  cacheStatus = entry->cacheStatus;
  return true;
}

void updateBookListStatusIndex(const std::string& filepath, const ReadingStatus readingStatus,
                               const CachedBookStatus cacheStatus, std::vector<BookListStatusEntry>& entries) {
  std::string cacheEntryName;
  bool isEpub = false;
  if (!getCacheEntryName(filepath, cacheEntryName, isEpub)) return;
  const auto entry = std::lower_bound(entries.begin(), entries.end(), cacheEntryName,
                                      [](const BookListStatusEntry& value, const std::string& name) {
                                        return value.cacheEntryName < name;
                                      });
  if (entry != entries.end() && entry->cacheEntryName == cacheEntryName) {
    entry->readingStatus = readingStatus;
    entry->cacheStatus = cacheStatus;
    return;
  }
  entries.insert(entry, {std::move(cacheEntryName), readingStatus, cacheStatus});
}

void moveBookListStatusIndexEntry(const std::string& oldPath, const std::string& newPath,
                                  std::vector<BookListStatusEntry>& entries) {
  std::string oldEntryName;
  bool isEpub = false;
  if (!getCacheEntryName(oldPath, oldEntryName, isEpub)) return;
  const auto entry = std::lower_bound(entries.begin(), entries.end(), oldEntryName,
                                      [](const BookListStatusEntry& value, const std::string& name) {
                                        return value.cacheEntryName < name;
                                      });
  if (entry == entries.end() || entry->cacheEntryName != oldEntryName) return;
  const ReadingStatus readingStatus = entry->readingStatus;
  entries.erase(entry);
  // Generated cache data is path-keyed and remains at the old location.
  updateBookListStatusIndex(newPath, readingStatus, CachedBookStatus::Unknown, entries);
}

void removeBookListStatusIndexEntry(const std::string& filepath, std::vector<BookListStatusEntry>& entries) {
  std::string cacheEntryName;
  bool isEpub = false;
  if (!getCacheEntryName(filepath, cacheEntryName, isEpub)) return;
  const auto entry = std::lower_bound(entries.begin(), entries.end(), cacheEntryName,
                                      [](const BookListStatusEntry& value, const std::string& name) {
                                        return value.cacheEntryName < name;
                                      });
  if (entry != entries.end() && entry->cacheEntryName == cacheEntryName) entries.erase(entry);
}

void invalidateBookListStatusIndexEntry(const std::string& filepath, const std::string& cacheDir) {
  std::vector<BookListStatusEntry> entries;
  loadBookListStatusIndex(cacheDir, entries);
  const size_t before = entries.size();
  removeBookListStatusIndexEntry(filepath, entries);
  if (entries.size() != before) saveBookListStatusIndex(cacheDir, entries);
}

bool saveBookListStatusIndex(const std::string& cacheDir, const std::vector<BookListStatusEntry>& entries) {
  if (entries.size() > BOOK_LIST_STATUS_INDEX_MAX_ENTRIES) return false;
  const std::string indexPath = cacheDir + BOOK_LIST_STATUS_INDEX_FILE;
  const std::string tempPath = cacheDir + BOOK_LIST_STATUS_INDEX_TMP_FILE;
  Storage.remove(tempPath.c_str());
  FsFile file;
  if (!Storage.openFileForWrite("BLI", tempPath, file)) return false;

  const uint16_t count = static_cast<uint16_t>(entries.size());
  bool written = file.write(&BOOK_LIST_STATUS_INDEX_MAGIC, sizeof(BOOK_LIST_STATUS_INDEX_MAGIC)) ==
                     sizeof(BOOK_LIST_STATUS_INDEX_MAGIC) &&
                 file.write(&BOOK_LIST_STATUS_INDEX_VERSION, sizeof(BOOK_LIST_STATUS_INDEX_VERSION)) ==
                     sizeof(BOOK_LIST_STATUS_INDEX_VERSION) &&
                 file.write(&count, sizeof(count)) == sizeof(count);
  for (const auto& entry : entries) {
    const uint8_t nameLength = static_cast<uint8_t>(entry.cacheEntryName.size());
    const uint8_t reading = static_cast<uint8_t>(entry.readingStatus);
    const uint8_t cache = static_cast<uint8_t>(entry.cacheStatus);
    if (nameLength == 0 || nameLength > BOOK_LIST_STATUS_INDEX_MAX_NAME_LENGTH ||
        file.write(&nameLength, sizeof(nameLength)) != sizeof(nameLength) ||
        file.write(entry.cacheEntryName.data(), nameLength) != nameLength || file.write(&reading, sizeof(reading)) != sizeof(reading) ||
        file.write(&cache, sizeof(cache)) != sizeof(cache)) {
      written = false;
      break;
    }
  }
  file.flush();
  file.close();
  if (!written) {
    Storage.remove(tempPath.c_str());
    return false;
  }
  Storage.remove(indexPath.c_str());
  if (!Storage.rename(tempPath.c_str(), indexPath.c_str())) {
    Storage.remove(tempPath.c_str());
    return false;
  }
  LOG_DBG("BLI", "Saved book-list status index: entries=%u", count);
  return true;
}

ReadingStatus getReadingStatus(const std::string& filepath, const std::string& cacheDir, const uint64_t bookId) {
  std::string cacheEntryName;
  bool isEpub;
  if (!getCacheEntryName(filepath, cacheEntryName, isEpub)) {
    return ReadingStatus::Unread;
  }

  if (isEpub) {
    // Recent/history records created before BookId support have no stored ID.
    // The path index is a cheap bridge in that case; it lets those records
    // find their canonical progress without opening the EPUB on the Home UI.
    uint64_t resolvedBookId = bookId;
    if (resolvedBookId == 0) BookIdentity::getLastArchiveId(filepath, resolvedBookId);
    if (resolvedBookId != 0) {
      const std::string canonicalPath = BookDataPath::getProgressPath(resolvedBookId);
      if (Storage.exists(canonicalPath.c_str())) return readProgress(canonicalPath, true);
    }
  }

  return readProgress(cacheDir + "/" + cacheEntryName + "/progress.bin", isEpub);
}

void getReadingStatusCacheEntries(const std::string& cacheDir, std::vector<std::string>& cacheEntries) {
  const unsigned long startedAt = millis();
  cacheEntries.clear();

  uint32_t scannedEntries = 0;
  bool cacheRootOpen = false;
  bool cacheRootIsDirectory = false;
  {
    auto cacheRoot = Storage.open(cacheDir.c_str());
    cacheRootOpen = static_cast<bool>(cacheRoot);
    if (cacheRootOpen && cacheRoot.isDirectory()) {
      cacheRootIsDirectory = true;
      char name[128];
      for (auto entry = cacheRoot.openNextFile(); entry; entry = cacheRoot.openNextFile()) {
        entry.getName(name, sizeof(name));
        cacheEntries.emplace_back(name);
        ++scannedEntries;
      }
    }
  }
  std::sort(cacheEntries.begin(), cacheEntries.end());

  LOG_DBG("RSH", "Status cache index: cacheEntries=%lu openNext=%lu getName=%lu isDirectory=%lu close=%lu total=%lu ms",
          static_cast<unsigned long>(scannedEntries),
          static_cast<unsigned long>(cacheRootIsDirectory ? scannedEntries + 1 : 0),
          static_cast<unsigned long>(scannedEntries), static_cast<unsigned long>(cacheRootOpen ? 1 : 0),
          static_cast<unsigned long>(cacheRootIsDirectory ? scannedEntries + 2 : 1), millis() - startedAt);
}

bool hasBookCacheEntry(const std::string& filepath, const std::vector<std::string>& cacheEntries) {
  std::string cacheEntryName;
  bool isEpub = false;
  return getCacheEntryName(filepath, cacheEntryName, isEpub) && cacheEntryExists(cacheEntries, cacheEntryName);
}

bool getReadingStatusFromCacheEntries(const std::string& filepath, const std::string& cacheDir,
                                      const std::vector<std::string>& cacheEntries, ReadingStatus& status) {
  std::string cacheEntryName;
  bool isEpub;
  if (!getCacheEntryName(filepath, cacheEntryName, isEpub) ||
      !std::binary_search(cacheEntries.begin(), cacheEntries.end(), cacheEntryName)) {
    status = ReadingStatus::Unread;
    return false;
  }

  status = readProgress(cacheDir + "/" + cacheEntryName + "/progress.bin", isEpub);
  return true;
}

void getReadingStatuses(const std::string& basePath, const std::vector<std::string>& filenames,
                        const std::string& cacheDir, std::vector<ReadingStatus>& statuses) {
  const unsigned long startedAt = millis();
  statuses.assign(filenames.size(), ReadingStatus::Unread);

  std::vector<std::string> cacheEntries;
  getReadingStatusCacheEntries(cacheDir, cacheEntries);

  std::string fullBase = basePath;
  if (fullBase.back() != '/') fullBase += '/';

  uint32_t books = 0;
  uint32_t progressReads = 0;
  for (size_t i = 0; i < filenames.size(); ++i) {
    const std::string filepath = fullBase + filenames[i];
    ReadingStatus status;
    const bool hasCacheEntry = getReadingStatusFromCacheEntries(filepath, cacheDir, cacheEntries, status);
    if (!FsHelpers::hasEpubExtension(filenames[i]) && !FsHelpers::hasXtcExtension(filenames[i]) &&
        !FsHelpers::hasTxtExtension(filenames[i]) && !FsHelpers::hasMarkdownExtension(filenames[i])) continue;
    ++books;
    statuses[i] = status;
    if (hasCacheEntry) ++progressReads;
  }

  LOG_DBG("RSH", "Status lookup: books=%lu progressReads=%lu total=%lu ms", static_cast<unsigned long>(books),
          static_cast<unsigned long>(progressReads), millis() - startedAt);
}

bool markAsFinished(const std::string& filepath, const std::string& cacheDir) {
  const char* prefix;
  bool isEpub;
  if (FsHelpers::hasEpubExtension(filepath)) {
    prefix = "epub_";
    isEpub = true;
  } else if (FsHelpers::hasXtcExtension(filepath)) {
    prefix = "xtc_";
    isEpub = false;
  } else if (FsHelpers::hasTxtExtension(filepath) || FsHelpers::hasMarkdownExtension(filepath)) {
    prefix = "txt_";
    isEpub = false;
  } else {
    return false;
  }

  const std::string hash = std::to_string(std::hash<std::string>{}(filepath));
  const std::string legacyProgressPath = cacheDir + "/" + prefix + hash + "/progress.bin";
  uint64_t bookId = 0;
  const bool hasBookId = isEpub && Epub(filepath, cacheDir).getSourceFingerprint(&bookId);
  const std::string progressPath = hasBookId ? BookDataPath::getProgressPath(bookId) : legacyProgressPath;

  // EPUB=7, XTC/TXT=5
  const size_t recordSize = isEpub ? 7 : 5;
  const size_t flagOffset = isEpub ? 6 : 4;

  // 既存progress.binを読み込んで読書位置を保持する（なければゼロ初期化）
  uint8_t data[7] = {0};
  const std::string sourcePath = Storage.exists(progressPath.c_str()) ? progressPath : legacyProgressPath;
  FsFile rf;
  if (Storage.openFileForRead("RSH", sourcePath, rf)) {
    rf.read(data, recordSize);
    rf.close();
  }
  data[flagOffset] = 1;

  if (hasBookId && (!BookDataPath::ensureDirectory(bookId) || !BookIdentity::recordArchiveId(filepath, bookId))) return false;
  if (!hasBookId) {
    Storage.mkdir(cacheDir.c_str());
    Storage.mkdir((cacheDir + "/" + prefix + hash).c_str());
  }
  if (!ProgressFile::writeAtomicPath(progressPath, data, recordSize)) return false;
  READING_HISTORY.markFinished(filepath, hasBookId ? bookId : 0);
  LOG_DBG("RSH", "Marked as finished: %s", filepath.c_str());
  return true;
}
