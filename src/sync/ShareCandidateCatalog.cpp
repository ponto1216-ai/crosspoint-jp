#include "ShareCandidateCatalog.h"

#include <algorithm>
#include <cstring>

#include "BookReaderSettings.h"
#include "ReadingHistoryStore.h"

namespace yomuka::sync {
bool ShareCandidateCatalog::parseBookId(const char* key, uint64_t& id) {
  if (!key || std::strlen(key) != 16) return false;
  uint64_t parsed = 0;
  for (unsigned i = 0; i < 16; ++i) {
    const char c = key[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    parsed = (parsed << 4) | static_cast<uint64_t>(c <= '9' ? c - '0' : c - 'a' + 10);
  }
  if (!parsed) return false;
  id = parsed;
  return true;
}
ShareCandidate& ShareCandidateCatalog::obtain(uint64_t id) {
  auto it = std::lower_bound(entries.begin(), entries.end(), id,
                             [](const ShareCandidate& item, uint64_t key) { return item.bookId < key; });
  if (it == entries.end() || it->bookId != id) {
    ShareCandidate item;
    item.bookId = id;
    it = entries.insert(it, std::move(item));
  }
  return *it;
}
void ShareCandidateCatalog::observe(uint64_t id, uint8_t unit, bool valid, bool empty) {
  if (!id) return;
  auto& item = obtain(id);
  item.units |= unit;
  if (!valid) item.errors |= unit;
  if (empty && valid) item.emptyUnits |= unit;
}
bool ShareCandidateCatalog::collectHistory(const JsonDocument& document, size_t& legacyCount) {
  std::vector<ReadingHistoryBook> books;
  std::vector<uint64_t> removed;
  if (ReadingHistoryStore::decodeSavedBooks(document, books, removed) != ReadStatus::Present) return false;
  uint16_t rank = 0;
  for (const auto& book : books) {
    if (!book.bookId) {
      ++legacyCount;
      ++rank;
      continue;
    }
    auto& item = obtain(book.bookId);
    // Duplicate IDs cannot be exported by readForSync; make the problem visible.
    if (item.units & SavedHistory) item.errors |= SavedHistory;
    item.units |= SavedHistory;
    if (rank < item.historyRank) {
      item.historyRank = rank;
      item.path = book.path;
      item.title = book.title;
    }
    ++rank;
  }
  for (uint64_t id : removed) {
    auto& item = obtain(id);
    if (item.units & SavedHistory) item.errors |= SavedHistory;
    item.units |= SavedHistory;
    item.emptyUnits |= SavedHistory;
  }
  return true;
}
bool ShareCandidateCatalog::collectSettings(const JsonDocument& document) {
  if (!document["formatVersion"].is<uint8_t>() || document["formatVersion"].as<uint8_t>() != 1 ||
      !document["books"].is<JsonObjectConst>())
    return false;
  bool valid = true;
  for (JsonPairConst entry : document["books"].as<JsonObjectConst>()) {
    uint64_t id = 0;
    if (entry.key().size() != 16 || !parseBookId(entry.key().c_str(), id)) {
      valid = false;
      continue;
    }
    BookReaderSettings::Override value;
    uint32_t date = 0;
    const bool decoded = BookReaderSettings::decodeSavedOverride(entry.value().as<JsonObjectConst>(), value, date);
    observe(id, SavedSettings, decoded, !BookReaderSettings::hasAnyField(value));
  }
  return valid;
}
bool ShareCandidateCatalog::collectPaths(const JsonDocument& document) {
  if (!document["formatVersion"].is<uint8_t>() || document["formatVersion"].as<uint8_t>() != 1 ||
      !document["paths"].is<JsonObjectConst>())
    return false;
  bool valid = true;
  for (JsonPairConst entry : document["paths"].as<JsonObjectConst>()) {
    uint64_t id = 0;
    if (!parseBookId(entry.value().as<const char*>(), id)) {
      valid = false;
      continue;
    }
    auto it = std::lower_bound(entries.begin(), entries.end(), id,
                               [](const ShareCandidate& item, uint64_t key) { return item.bookId < key; });
    // A locator alone is not reading data.
    if (it == entries.end() || it->bookId != id || it->historyRank != std::numeric_limits<uint16_t>::max()) continue;
    const std::string path = entry.key().c_str();
    if (path.empty() || path.front() != '/' || path.find('\0') != std::string::npos) {
      valid = false;
      continue;
    }
    if (it->path.empty() || path < it->path) it->path = path;
  }
  return valid;
}
void ShareCandidateCatalog::resolve(uint64_t id, const std::string& path, uint8_t priority) {
  auto it = std::lower_bound(entries.begin(), entries.end(), id,
                             [](const ShareCandidate& item, uint64_t key) { return item.bookId < key; });
  if (it == entries.end() || it->bookId != id) return;
  if (priority < it->locationPriority || (priority == it->locationPriority && path < it->path)) {
    it->path = path;
    it->locationPriority = priority;
  }
}
size_t ShareCandidateCatalog::missingCount() const {
  return std::count_if(entries.begin(), entries.end(), [](const ShareCandidate& item) {
    // A history deletion tombstone remains available for sharing, but by
    // itself should not make an absent EPUB count as a missing book.
    const bool deletionOnly = item.units == SavedHistory && item.emptyUnits == SavedHistory && item.errors == 0;
    return !item.resolved() && !deletionOnly;
  });
}
size_t ShareCandidateCatalog::unresolvedCount() const {
  return std::count_if(entries.begin(), entries.end(), [](const ShareCandidate& item) { return !item.resolved(); });
}
void ShareCandidateCatalog::finish() {
  entries.erase(
      std::remove_if(entries.begin(), entries.end(), [](const ShareCandidate& item) { return !item.resolved(); }),
      entries.end());
  for (auto& item : entries) {
    if (item.title.empty()) item.title = item.path.substr(item.path.find_last_of('/') + 1);
  }
  std::sort(entries.begin(), entries.end(), [](const ShareCandidate& a, const ShareCandidate& b) {
    if (a.historyRank != b.historyRank) return a.historyRank < b.historyRank;
    if (a.title != b.title) return a.title < b.title;
    if (a.path != b.path) return a.path < b.path;
    return a.bookId < b.bookId;
  });
}
}  // namespace yomuka::sync
