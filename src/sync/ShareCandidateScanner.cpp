#include "ShareCandidateScanner.h"

#include <FsHelpers.h>
#include <ZipFile.h>

#include <algorithm>
#include <array>

#include "BookmarkStorage.h"
#include "ProgressStorage.h"
#include "SyncProgress.h"
#include "util/BookDataPath.h"

namespace yomuka::sync {
namespace {
constexpr size_t MaximumJsonBytes = 262144;
// ArduinoJson's custom reader avoids holding the serialized aggregate and DOM together.
struct FileReader {
  FsFile& file;
  size_t expected;
  std::array<uint8_t, 512> buffer{};
  size_t offset = 0, count = 0;
  bool failed = false;
  int read() {
    if (offset == count) {
      if (file.position() >= expected) return -1;
      const auto wanted = std::min(buffer.size(), expected - file.position());
      const int received = file.read(buffer.data(), wanted);
      if (received <= 0) {
        failed = true;
        return -1;
      }
      count = static_cast<size_t>(received);
      offset = 0;
    }
    return buffer[offset++];
  }
  size_t readBytes(char* data, size_t length) {
    size_t copied = 0;
    while (copied < length) {
      const int byte = read();
      if (byte < 0) break;
      data[copied++] = static_cast<char>(byte);
    }
    return copied;
  }
};
bool readDocument(const std::string& path, JsonDocument& document, size_t limit = MaximumJsonBytes) {
  auto file = Storage.open(path.c_str());
  if (!file || file.isDirectory() || !file.size() || file.size() > limit) return false;
  FileReader reader{file, file.size()};
  bool valid = !deserializeJson(document, reader) && !document.overflowed();
  // Reject an incomplete read or trailing non-JSON bytes, just like normal sync reads.
  if (valid) {
    int byte = 0;
    while ((byte = reader.read()) >= 0) {
      if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') valid = false;
    }
  }
  return file.close() && valid && !reader.failed;
}
bool fileName(FsFile& file, std::string& output) {
  std::array<char, 512> name{};
  const size_t length = file.getName(name.data(), name.size());
  if (!length || length >= name.size() - 1) return false;
  output = name.data();
  const auto slash = output.find_last_of('/');
  if (slash != std::string::npos) output.erase(0, slash + 1);
  return !output.empty();
}
}  // namespace
bool ShareCandidateScanner::excluded(uint64_t id) const {
  return std::find(excludedIds.begin(), excludedIds.end(), id) != excludedIds.end();
}
size_t ShareCandidateScanner::searchableCount() const {
  return std::count_if(catalog.books().begin(), catalog.books().end(),
                       [this](const ShareCandidate& item) { return !item.resolved() && !excluded(item.bookId); });
}
void ShareCandidateScanner::aggregate(const char* path, Phase source) {
  if (!Storage.exists(path)) {
    // Do not repair interrupted writes during discovery, or call them an empty dataset.
    if (Storage.exists((std::string(path) + ".bak").c_str()) || Storage.exists((std::string(path) + ".tmp").c_str()))
      ++warnings;
    return;
  }
  JsonDocument document;
  if (!readDocument(path, document)) {
    ++warnings;
    return;
  }
  bool valid = false;
  if (source == Phase::History) valid = catalog.collectHistory(document, legacyCount);
  if (source == Phase::Settings) valid = catalog.collectSettings(document);
  if (source == Phase::Paths) valid = catalog.collectPaths(document);
  if (!valid) ++warnings;
}
void ShareCandidateScanner::inspectBookData(uint64_t id) {
  const auto progressPath = BookDataPath::getProgressPath(id);
  if (Storage.exists(progressPath.c_str())) {
    auto file = Storage.open(progressPath.c_str());
    uint8_t bytes[8]{};
    Progress progress;
    const size_t length = file ? file.size() : 0;
    bool valid = file && length <= sizeof(bytes) && file.read(bytes, length) == static_cast<int>(length) &&
                 decodeLegacyProgress(bytes, length, progress) == ProgressError::None;
    if (file && !file.close()) valid = false;
    const auto metadataPath = progressPath + ".sync-time";
    if (valid && Storage.exists(metadataPath.c_str())) {
      auto metadataFile = Storage.open(metadataPath.c_str());
      uint8_t metadata[30]{};
      uint32_t timestamp = 0;
      valid = metadataFile && metadataFile.size() == sizeof(metadata) &&
              metadataFile.read(metadata, sizeof(metadata)) == static_cast<int>(sizeof(metadata)) &&
              decodeSavedProgressTime(bytes, length, metadata, sizeof(metadata), timestamp);
      if (metadataFile && !metadataFile.close()) valid = false;
    }
    catalog.observe(id, SavedPosition, valid);
  } else if (Storage.exists((progressPath + ".bak").c_str()))
    catalog.observe(id, SavedPosition, false);
  const auto bookmarkPath = BookDataPath::getBookmarkPath(id);
  if (Storage.exists(bookmarkPath.c_str())) {
    JsonDocument document;
    std::vector<BookmarkEntry> bookmarks;
    uint32_t date = 0;
    const bool valid = readDocument(bookmarkPath, document, 65536) &&
                       decodeSavedBookmarks(document.as<JsonObjectConst>(), bookmarks, date);
    catalog.observe(id, SavedBookmarks, valid, bookmarks.empty());
  } else if (Storage.exists((bookmarkPath + ".bak").c_str()))
    catalog.observe(id, SavedBookmarks, false);
}
void ShareCandidateScanner::checkEpub(const std::string& path, uint64_t expectedId) {
  // Do not explore system directories even if an old locator points into one.
  if (path.empty() || path.front() != '/' || path.find("/.") != std::string::npos) return;
  if (!FsHelpers::hasEpubExtension(path) || !Storage.exists(path.c_str())) return;
  ++checkedFiles;
  uint64_t id = 0;
  ZipFile zip(path);
  if (!zip.getArchiveFingerprint(&id) || !id) {
    ++warnings;
    return;
  }
  // A same-path replacement is never resolved as the old book.
  if (!excluded(id)) catalog.resolve(id, path, id == expectedId ? 0 : 1);
}
bool ShareCandidateScanner::step() {
  if (done()) return true;
  if (!Storage.ready()) {
    ++warnings;
    complete();
    return true;
  }
  switch (phase) {
    case Phase::History:
      aggregate("/.crosspoint/reading-history.json", phase);
      phase = Phase::Settings;
      break;
    case Phase::Settings:
      aggregate("/.crosspoint/book-reader-settings.json", phase);
      if (Storage.exists("/.crosspoint/books")) {
        bookDirectory = Storage.open("/.crosspoint/books");
        if (!bookDirectory || !bookDirectory.isDirectory()) ++warnings;
      }
      phase = Phase::BookData;
      break;
    case Phase::BookData: {
      auto entry = bookDirectory ? bookDirectory.openNextFile() : FsFile{};
      if (!entry) {
        bookDirectory.close();
        phase = Phase::Paths;
        break;
      }
      if (entry.isDirectory()) {
        std::string name;
        uint64_t id = 0;
        if (fileName(entry, name) && ShareCandidateCatalog::parseBookId(name.c_str(), id))
          inspectBookData(id);
        else
          ++warnings;
      }
      break;
    }
    case Phase::Paths:
      aggregate("/.crosspoint/book-identity-paths.json", phase);
      phase = Phase::KnownPaths;
      break;
    case Phase::KnownPaths:
      if (knownIndex < catalog.books().size()) {
        // Copy before resolve() can replace a locator in the same vector.
        const auto id = catalog.books()[knownIndex].bookId;
        const auto path = catalog.books()[knownIndex++].path;
        if (!excluded(id)) checkEpub(path, id);
      } else if (searchMissing && searchableCount()) {
        auto root = Storage.open("/");
        if (root && root.isDirectory())
          directories.push_back({std::move(root), ""});
        else
          ++warnings;
        phase = Phase::Search;
      } else {
        phase = Phase::Search;
      }
      break;
    case Phase::Search: {
      if (directories.empty()) {
        complete();
        break;
      }
      auto file = directories.back().file.openNextFile();
      if (!file) {
        directories.pop_back();
        break;
      }
      std::string name;
      if (!fileName(file, name)) {
        ++warnings;
        break;
      }
      if (name.front() == '.') break;
      const auto path = directories.back().path + "/" + name;
      if (file.isDirectory()) {
        // Bound the number of simultaneous directory handles. Report incomplete searches.
        if (directories.size() >= 32)
          ++warnings;
        else
          directories.push_back({std::move(file), path});
      } else if (FsHelpers::hasEpubExtension(path)) {
        file.close();
        checkEpub(path);
      }
      break;
    }
    case Phase::Done:
      break;
  }
  return done();
}
void ShareCandidateScanner::complete() {
  for (const auto& item : catalog.books()) {
    if (item.resolved()) continue;
    unresolvedRecords.push_back({item.bookId, item.deletionOnly()});
    if (searchMissing || excluded(item.bookId)) missingCandidates.push_back(item);
  }
  setExclusions(excludedIds);
  catalog.finish();
  warnings += std::count_if(catalog.books().begin(), catalog.books().end(),
                            [](const ShareCandidate& book) { return book.errors != 0; });
  phase = Phase::Done;
}
void ShareCandidateScanner::restart(bool fullSearch) {
  auto next = ShareCandidateScanner(fullSearch);
  next.setExclusions(excludedIds);
  *this = std::move(next);
}
void ShareCandidateScanner::setExclusions(std::vector<uint64_t> ids) {
  excludedIds = std::move(ids);
  unresolvedBooks = 0;
  missingBooks = 0;
  for (const auto& item : unresolvedRecords) {
    if (excluded(item.bookId)) continue;
    ++unresolvedBooks;
    if (!item.deletionOnly) ++missingBooks;
  }
}
}  // namespace yomuka::sync
