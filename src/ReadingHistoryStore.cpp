#include "ReadingHistoryStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {
constexpr char HISTORY_PATH[] = "/.crosspoint/reading-history.json";
constexpr uint32_t MIN_VALID_UNIX_TIME = 1704067200;  // 2024-01-01
constexpr unsigned long INACTIVITY_TIMEOUT_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long SAVE_INTERVAL_MS = 60UL * 1000UL;
constexpr size_t MAX_BOOKS = 100;
constexpr uint32_t MIN_TOP_BOOK_SECONDS = 60;
constexpr size_t MAX_DAYS = 366;

bool optionalU32(JsonObjectConst object, const char* key, uint32_t& value) {
  if (object[key].isUnbound()) return true;
  if (!object[key].is<uint32_t>()) return false;
  value = object[key].as<uint32_t>();
  return true;
}
bool parseBookId(const char* key, uint64_t& bookId) {
  if (!key || strlen(key) != 16) return false;
  uint64_t result = 0;
  for (unsigned i = 0; i < 16; ++i) {
    const char digit = key[i];
    if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) return false;
    result = (result << 4U) | static_cast<uint64_t>(digit <= '9' ? digit - '0' : digit - 'a' + 10);
  }
  bookId = result;
  return true;
}
bool readBook(JsonVariantConst item, ReadingHistoryBook& book) {
  if (!item.is<JsonObjectConst>()) return false;
  const auto object = item.as<JsonObjectConst>();
  if (!object["path"].is<const char*>() || !object["title"].is<const char*>() || !object["author"].is<const char*>() ||
      !object["seconds"].is<uint32_t>())
    return false;
  book.path = object["path"].as<std::string>();
  book.title = object["title"].as<std::string>();
  book.author = object["author"].as<std::string>();
  if (book.path.empty() || book.path.find('\0') != std::string::npos) return false;
  book.seconds = object["seconds"].as<uint32_t>();
  if (!object["bookId"].isUnbound() &&
      (!object["bookId"].is<const char*>() || object["bookId"].as<JsonString>().size() != 16 ||
       !parseBookId(object["bookId"].as<const char*>(), book.bookId)))
    return false;
  if (!optionalU32(object, "lastReadAt", book.lastReadAt) || !optionalU32(object, "finishedAt", book.finishedAt) ||
      !optionalU32(object, "sessionCount", book.sessionCount) || !yomuka::sync::readUpdateTime(object, book.updatedAt))
    return false;
  if (!object["finished"].isUnbound() && !object["finished"].is<bool>()) return false;
  book.finished = object["finished"] | (book.finishedAt != 0);
  return true;
}
yomuka::sync::ReadStatus validateHistory(const JsonDocument& document) {
  using yomuka::sync::ReadStatus;
  if (!document["version"].is<uint8_t>() ||
      (document["version"].as<uint8_t>() != 1 && document["version"].as<uint8_t>() != 2) ||
      !document["totalSeconds"].is<uint32_t>() || !document["books"].is<JsonArrayConst>() ||
      !document["days"].is<JsonArrayConst>())
    return ReadStatus::Corrupt;
  if (document["books"].size() > MAX_BOOKS || document["days"].size() > MAX_DAYS) return ReadStatus::LimitExceeded;
  for (JsonVariantConst item : document["books"].as<JsonArrayConst>()) {
    ReadingHistoryBook book;
    if (!readBook(item, book)) return ReadStatus::Corrupt;
  }
  for (JsonVariantConst day : document["days"].as<JsonArrayConst>()) {
    if (!day.is<JsonObjectConst>() || !day["date"].is<uint32_t>() || day["date"].as<uint32_t>() == 0 ||
        !day["seconds"].is<uint32_t>())
      return ReadStatus::Corrupt;
  }
  const auto removed = document["removedBooks"];
  if (!removed.isUnbound()) {
    if (!removed.is<JsonObjectConst>()) return ReadStatus::Corrupt;
    for (JsonPairConst entry : removed.as<JsonObjectConst>()) {
      uint64_t bookId = 0;
      if (entry.key().size() != 16 || !parseBookId(entry.key().c_str(), bookId) || bookId == 0 ||
          !entry.value().is<uint32_t>())
        return ReadStatus::Corrupt;
    }
  }
  return ReadStatus::Present;
}
}  // namespace

ReadingHistoryStore ReadingHistoryStore::instance;
// Share-candidate discovery reads this aggregate once, rather than once per book.
yomuka::sync::ReadStatus ReadingHistoryStore::decodeSavedBooks(const JsonDocument& document,
                                                               std::vector<ReadingHistoryBook>& output,
                                                               std::vector<uint64_t>& removed) {
  const auto status = validateHistory(document);
  if (status != yomuka::sync::ReadStatus::Present) return status;
  std::vector<ReadingHistoryBook> candidate;
  std::vector<uint64_t> deletions;
  for (JsonVariantConst entry : document["books"].as<JsonArrayConst>()) {
    ReadingHistoryBook book;
    if (!readBook(entry, book)) return yomuka::sync::ReadStatus::Corrupt;
    candidate.push_back(std::move(book));
  }
  for (JsonPairConst entry : document["removedBooks"].as<JsonObjectConst>()) {
    uint64_t id = 0;
    if (!parseBookId(entry.key().c_str(), id)) return yomuka::sync::ReadStatus::Corrupt;
    deletions.push_back(id);
  }
  output = std::move(candidate);
  removed = std::move(deletions);
  return yomuka::sync::ReadStatus::Present;
}

bool ReadingHistoryStore::prepareForSync(const ReadingHistoryBook& book, uint32_t updatedAt, JsonDocument& output) {
  HalStorage::StorageLock lock;
  if (!activePath.empty() || dirty || !book.bookId) return false;
  JsonDocument document;
  const auto status = yomuka::sync::readJson(HISTORY_PATH, document);
  if (status == yomuka::sync::ReadStatus::Absent) {
    document["version"] = 2;
    document["totalSeconds"] = 0;
    document["books"].to<JsonArray>();
    document["days"].to<JsonArray>();
  } else if (status != yomuka::sync::ReadStatus::Present ||
             validateHistory(document) != yomuka::sync::ReadStatus::Present)
    return false;
  document["version"] = 2;
  auto entries = document["books"].as<JsonArray>();
  JsonObject target;
  char key[17];
  snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(book.bookId));
  for (JsonObject item : entries) {
    const bool unresolved = item["bookId"].isUnbound() || item["bookId"] == "0000000000000000";
    if (item["bookId"] == key || (unresolved && item["path"] == book.path)) {
      if (!target.isNull()) return false;
      target = item;
    }
  }
  if (target.isNull()) {
    if (entries.size() >= MAX_BOOKS) return false;
    target = entries.add<JsonObject>();
  }
  target.clear();
  target["bookId"] = key;
  target["path"] = book.path;
  target["title"] = book.title;
  target["author"] = book.author;
  target["seconds"] = book.seconds;
  target["sessionCount"] = book.sessionCount;
  target["lastReadAt"] = book.lastReadAt;
  target["finishedAt"] = book.finishedAt;
  target["finished"] = book.finished;
  target["updatedAt"] = updatedAt;
  document["removedBooks"].as<JsonObject>().remove(key);
  if (document.overflowed()) return false;
  output = std::move(document);
  return true;
}
bool ReadingHistoryStore::reloadAfterSync() {
  if (!activePath.empty() || dirty) return false;
  books.clear();
  days.clear();
  removedBooks.clear();
  totalSeconds = 0;
  loaded = true;
  return loadFromFile() || loadStatus == yomuka::sync::ReadStatus::Absent;
}

void ReadingHistoryStore::ensureLoaded() {
  if (loaded) return;
  loaded = true;
  if (!loadFromFile()) {
    books.clear();
    days.clear();
    totalSeconds = 0;
  }
}

uint32_t ReadingHistoryStore::currentDate() const {
  const time_t now = time(nullptr);
  if (now < MIN_VALID_UNIX_TIME) return 0;
  struct tm localTime{};
  localtime_r(&now, &localTime);
  return static_cast<uint32_t>((localTime.tm_year + 1900) * 10000 + (localTime.tm_mon + 1) * 100 + localTime.tm_mday);
}

uint32_t ReadingHistoryStore::currentTimestamp() const { return yomuka::sync::localUpdateTime(); }

void ReadingHistoryStore::noteBookChange(ReadingHistoryBook& book) {
  book.pendingUpdatedAt = currentTimestamp();
  book.timestampDirty = true;
  dirty = true;
}

void ReadingHistoryStore::beginSession(const std::string& path, const std::string& title, const std::string& author,
                                       const uint64_t bookId) {
  ensureLoaded();
  endSession();
  activePath = path;
  activeBookId = bookId;
  if (bookId != 0) {
    removedBooks.erase(std::remove_if(removedBooks.begin(), removedBooks.end(),
                                      [bookId](const RemovedBook& entry) { return entry.bookId == bookId; }),
                       removedBooks.end());
  }
  const auto it = std::find_if(books.begin(), books.end(), [&path, bookId](const ReadingHistoryBook& entry) {
    return entry.path == path || (bookId != 0 && entry.bookId == bookId);
  });
  if (it == books.end()) {
    books.insert(books.begin(), {path, title, author, 0, bookId});
    if (books.size() > MAX_BOOKS) books.resize(MAX_BOOKS);
    dirty = true;
  } else {
    ReadingHistoryBook updated = *it;
    updated.path = path;
    updated.title = title;
    updated.author = author;
    if (bookId != 0) updated.bookId = bookId;
    books.erase(it);
    books.insert(books.begin(), updated);
  }
  const uint32_t nowTimestamp = currentTimestamp();
  if (!books.empty()) {
    auto active = std::find_if(books.begin(), books.end(), [this](const ReadingHistoryBook& entry) {
      return entry.path == activePath || (activeBookId != 0 && entry.bookId == activeBookId);
    });
    if (active != books.end()) {
      ++active->sessionCount;
      if (nowTimestamp != 0) active->lastReadAt = nowTimestamp;
      noteBookChange(*active);
    }
  }
  dirty = true;
  const unsigned long now = millis();
  lastTickMs = now;
  lastInteractionMs = now;
  lastSaveMs = now;
  pendingMilliseconds = 0;
  LOG_DBG("RH", "Started reading session: %s", path.c_str());
}

bool ReadingHistoryStore::markFinished(const std::string& path, const uint64_t bookId) {
  ensureLoaded();
  auto it = std::find_if(books.begin(), books.end(), [&path, bookId](const ReadingHistoryBook& entry) {
    return entry.path == path || (bookId != 0 && entry.bookId == bookId);
  });
  if (it == books.end()) {
    const size_t slash = path.find_last_of('/');
    const std::string title = slash == std::string::npos ? path : path.substr(slash + 1);
    books.insert(books.begin(), {path, title, "", 0, bookId});
    it = books.begin();
    if (books.size() > MAX_BOOKS) books.resize(MAX_BOOKS);
  } else {
    it->path = path;
    if (bookId != 0) it->bookId = bookId;
  }
  const uint32_t nowTimestamp = currentTimestamp();
  const bool changed = !it->finished || (nowTimestamp != 0 && it->lastReadAt != nowTimestamp);
  it->finished = true;
  if (nowTimestamp != 0 && it->finishedAt == 0) it->finishedAt = nowTimestamp;
  if (nowTimestamp != 0) it->lastReadAt = nowTimestamp;
  if (changed) noteBookChange(*it);
  dirty = true;
  return saveToFile();
}

void ReadingHistoryStore::noteInteraction() {
  if (!activePath.empty()) lastInteractionMs = millis();
}

void ReadingHistoryStore::addSeconds(const uint32_t seconds) {
  if (seconds == 0 || activePath.empty()) return;
  totalSeconds += seconds;
  const auto it = std::find_if(books.begin(), books.end(), [this](const ReadingHistoryBook& entry) {
    return entry.path == activePath || (activeBookId != 0 && entry.bookId == activeBookId);
  });
  if (it != books.end()) {
    it->seconds += seconds;
    noteBookChange(*it);
  }

  const uint32_t date = currentDate();
  if (date != 0) {
    auto day = std::find_if(days.begin(), days.end(), [date](const DayEntry& entry) { return entry.date == date; });
    if (day == days.end()) {
      days.insert(days.begin(), {date, seconds});
      if (days.size() > MAX_DAYS) days.resize(MAX_DAYS);
    } else {
      day->seconds += seconds;
    }
  }
  dirty = true;
}

void ReadingHistoryStore::tick() {
  if (activePath.empty()) return;
  const unsigned long now = millis();
  const unsigned long elapsed = now - lastTickMs;
  lastTickMs = now;
  if (now - lastInteractionMs <= INACTIVITY_TIMEOUT_MS) {
    pendingMilliseconds += elapsed;
    if (pendingMilliseconds >= 1000) {
      addSeconds(pendingMilliseconds / 1000);
      pendingMilliseconds %= 1000;
    }
  }
  if (dirty && now - lastSaveMs >= SAVE_INTERVAL_MS) {
    saveToFile();
    lastSaveMs = now;
  }
}

bool ReadingHistoryStore::flushPending() {
  ensureLoaded();
  if (loadStatus != yomuka::sync::ReadStatus::Present && loadStatus != yomuka::sync::ReadStatus::Absent) return false;
  tick();
  return !dirty || saveToFile();
}

yomuka::sync::ReadStatus ReadingHistoryStore::readForSync(const uint64_t bookId, ReadingHistoryBook& result,
                                                          uint32_t& updatedAt) {
  using yomuka::sync::ReadStatus;
  HalStorage::StorageLock lock;
  if (bookId == 0) return ReadStatus::Corrupt;
  JsonDocument document;
  const auto status = yomuka::sync::readJson(HISTORY_PATH, document);
  if (status != ReadStatus::Present) return status;
  const auto validated = validateHistory(document);
  if (validated != ReadStatus::Present) return validated;
  ReadingHistoryBook candidate;
  bool found = false;
  for (JsonVariantConst entry : document["books"].as<JsonArrayConst>()) {
    ReadingHistoryBook book;
    if (!readBook(entry, book)) return ReadStatus::Corrupt;
    if (book.bookId != bookId) continue;
    if (found) return ReadStatus::Corrupt;  // Do not guess or add duplicate records.
    candidate = std::move(book);
    found = true;
  }
  if (!found) {
    char key[17];
    snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(bookId));
    const auto timestamp = document["removedBooks"][key];
    if (timestamp.isUnbound()) return ReadStatus::Absent;
    candidate.bookId = bookId;
    candidate.updatedAt = timestamp.as<uint32_t>();
  }
  result = std::move(candidate);
  updatedAt = result.updatedAt;
  return ReadStatus::Present;
}

bool ReadingHistoryStore::endSession() {
  const bool saved = flushPending();
  if (!activePath.empty()) LOG_DBG("RH", "Finished reading session: %s", activePath.c_str());
  activePath.clear();
  activeBookId = 0;
  pendingMilliseconds = 0;
  return saved;
}

bool ReadingHistoryStore::moveBook(const std::string& oldPath, const std::string& newPath) {
  ensureLoaded();
  const auto it = std::find_if(books.begin(), books.end(),
                               [&oldPath](const ReadingHistoryBook& entry) { return entry.path == oldPath; });
  if (it == books.end())
    return loadStatus == yomuka::sync::ReadStatus::Present || loadStatus == yomuka::sync::ReadStatus::Absent;
  it->path = newPath;
  if (activePath == oldPath) activePath = newPath;
  dirty = true;
  return saveToFile();
}

bool ReadingHistoryStore::migrateBookId(const uint64_t previousBookId, const uint64_t currentBookId) {
  if (previousBookId == 0 || currentBookId == 0) return false;
  if (previousBookId == currentBookId) return true;
  ensureLoaded();
  if (loadStatus != yomuka::sync::ReadStatus::Present && loadStatus != yomuka::sync::ReadStatus::Absent) return false;

  bool changed = false;
  for (auto& book : books) {
    if (book.bookId != previousBookId) continue;
    book.bookId = currentBookId;
    changed = true;
  }
  if (activeBookId == previousBookId) activeBookId = currentBookId;
  if (!changed) return true;

  // A reader session may already have created a record for the updated EPUB.
  // Merge it now so the persisted history, as well as the meter, has one book.
  std::vector<ReadingHistoryBook> merged;
  for (const auto& book : books) {
    const auto existing = std::find_if(merged.begin(), merged.end(), [&book](const ReadingHistoryBook& entry) {
      return entry.path == book.path || (book.bookId != 0 && entry.bookId == book.bookId);
    });
    if (existing == merged.end()) {
      merged.push_back(book);
    } else {
      existing->seconds += book.seconds;
      noteBookChange(*existing);
      existing->sessionCount += book.sessionCount;
      existing->lastReadAt = std::max(existing->lastReadAt, book.lastReadAt);
      existing->finished = existing->finished || book.finished;
      if (existing->finishedAt == 0 || (book.finishedAt != 0 && book.finishedAt < existing->finishedAt)) {
        existing->finishedAt = book.finishedAt;
      }
    }
  }
  books = std::move(merged);
  dirty = true;
  if (saveToFile()) {
    LOG_INF("RH", "Migrated reading history BookId %016llx -> %016llx", static_cast<unsigned long long>(previousBookId),
            static_cast<unsigned long long>(currentBookId));
    return true;
  }
  return false;
}

bool ReadingHistoryStore::removeBook(const std::string& path, const uint64_t bookId) {
  ensureLoaded();
  const auto it = std::find_if(books.begin(), books.end(), [&path, bookId](const ReadingHistoryBook& entry) {
    return entry.path == path || (bookId != 0 && entry.bookId == bookId);
  });
  if (it == books.end()) return false;

  const auto removed = *it;
  const auto previousRemovedBooks = removedBooks;
  if (removed.bookId != 0) {
    removedBooks.erase(std::remove_if(removedBooks.begin(), removedBooks.end(),
                                      [&removed](const RemovedBook& item) { return item.bookId == removed.bookId; }),
                       removedBooks.end());
    removedBooks.push_back({removed.bookId, currentTimestamp()});
  }
  const auto index = static_cast<size_t>(std::distance(books.begin(), it));
  books.erase(it);
  dirty = true;
  if (saveToFile()) {
    LOG_INF("RH", "Removed book from reading history: %s", path.c_str());
    return true;
  }

  books.insert(books.begin() + std::min(index, books.size()), removed);
  removedBooks = previousRemovedBooks;
  dirty = true;
  return false;
}

bool ReadingHistoryStore::clearAll() {
  ensureLoaded();
  const auto previousBooks = books;
  const auto previousDays = days;
  const uint32_t previousTotalSeconds = totalSeconds;
  const auto previousLoadStatus = loadStatus;
  const auto previousRemovedBooks = removedBooks;
  for (const auto& book : books) {
    if (book.bookId == 0) continue;
    removedBooks.erase(std::remove_if(removedBooks.begin(), removedBooks.end(),
                                      [&book](const RemovedBook& item) { return item.bookId == book.bookId; }),
                       removedBooks.end());
    removedBooks.push_back({book.bookId, currentTimestamp()});
  }

  books.clear();
  days.clear();
  totalSeconds = 0;
  loadStatus = yomuka::sync::ReadStatus::Absent;  // Explicit user-requested reset.
  dirty = true;
  if (saveToFile()) {
    LOG_INF("RH", "Cleared all reading history");
    return true;
  }

  books = previousBooks;
  days = previousDays;
  totalSeconds = previousTotalSeconds;
  loadStatus = previousLoadStatus;
  removedBooks = previousRemovedBooks;
  dirty = true;
  return false;
}

ReadingHistorySummary ReadingHistoryStore::getSummary() {
  ensureLoaded();
  tick();
  ReadingHistorySummary result;
  result.totalSeconds = totalSeconds;

  // Older path-only history may contain a stale entry after a move, and an
  // EPUB update can leave an old BookId beside its replacement. The meter is
  // a book-level view, so coalesce either representation before counting or
  // ranking it. totalSeconds intentionally remains the original session sum.
  std::vector<ReadingHistoryBook> summarizedBooks;
  for (const auto& book : books) {
    if (book.seconds == 0 && !book.finished) continue;
    const auto existing = std::find_if(summarizedBooks.begin(), summarizedBooks.end(), [&book](const auto& entry) {
      return entry.path == book.path || (book.bookId != 0 && entry.bookId == book.bookId);
    });
    if (existing == summarizedBooks.end()) {
      summarizedBooks.push_back(book);
    } else {
      existing->seconds += book.seconds;
      if (existing->bookId == 0 && book.bookId != 0) existing->bookId = book.bookId;
    }
  }

  for (const auto& book : summarizedBooks) {
    ++result.bookCount;
    if (book.finished) ++result.finishedBookCount;
    // The meter displays whole minutes. Do not rank a book as "most read"
    // when it would be shown as 0 minutes on either X3 or X4.
    if (book.seconds < MIN_TOP_BOOK_SECONDS) continue;
    for (size_t index = 0; index < result.topBooks.size(); ++index) {
      if (result.topBooks[index].seconds >= book.seconds) continue;
      for (size_t moveIndex = result.topBooks.size() - 1; moveIndex > index; --moveIndex) {
        result.topBooks[moveIndex] = result.topBooks[moveIndex - 1];
      }
      result.topBooks[index] = book;
      if (result.topBookCount < result.topBooks.size()) ++result.topBookCount;
      break;
    }
  }
  const time_t now = time(nullptr);
  result.hasCalendarTime = now >= MIN_VALID_UNIX_TIME;
  if (!result.hasCalendarTime) return result;

  const auto secondsForDate = [this](const uint32_t date) {
    const auto it =
        std::find_if(days.begin(), days.end(), [date](const DayEntry& entry) { return entry.date == date; });
    return it == days.end() ? 0U : it->seconds;
  };
  struct tm localTime{};
  localtime_r(&now, &localTime);
  const uint32_t todayDate =
      static_cast<uint32_t>((localTime.tm_year + 1900) * 10000 + (localTime.tm_mon + 1) * 100 + localTime.tm_mday);
  result.todaySeconds = secondsForDate(todayDate);

  // Monday is the first day of the weekly total. mktime() handles month/year
  // boundaries while keeping the local-time convention used for daily records.
  const int daysSinceMonday = (localTime.tm_wday + 6) % 7;
  for (int dayOffset = 0; dayOffset <= 6; ++dayOffset) {
    struct tm day = localTime;
    day.tm_mday -= daysSinceMonday - dayOffset;
    const time_t normalized = mktime(&day);
    localtime_r(&normalized, &day);
    const uint32_t date = static_cast<uint32_t>((day.tm_year + 1900) * 10000 + (day.tm_mon + 1) * 100 + day.tm_mday);
    result.weekSeconds += secondsForDate(date);
  }
  const uint32_t monthPrefix = static_cast<uint32_t>((localTime.tm_year + 1900) * 100 + localTime.tm_mon + 1);
  for (const auto& day : days) {
    if (day.date / 100 == monthPrefix) result.monthSeconds += day.seconds;
  }
  for (int index = 0; index < 7; ++index) {
    struct tm day = localTime;
    day.tm_mday -= 6 - index;
    const time_t normalized = mktime(&day);
    localtime_r(&normalized, &day);
    const uint32_t date = static_cast<uint32_t>((day.tm_year + 1900) * 10000 + (day.tm_mon + 1) * 100 + day.tm_mday);
    result.recentDaySeconds[index] = secondsForDate(date);
    result.recentDayWeekdays[index] = static_cast<uint8_t>(day.tm_wday);
  }
  return result;
}

const std::vector<ReadingHistoryBook>& ReadingHistoryStore::getBooks() {
  ensureLoaded();
  return books;
}

bool ReadingHistoryStore::saveToFile() {
  HalStorage::StorageLock lock;
  if ((loadStatus != yomuka::sync::ReadStatus::Present && loadStatus != yomuka::sync::ReadStatus::Absent) ||
      !Storage.ready() || !Storage.ensureDirectoryExists("/.crosspoint"))
    return false;
  JsonDocument doc;
  doc["version"] = 2;
  doc["totalSeconds"] = totalSeconds;
  JsonArray bookArray = doc["books"].to<JsonArray>();
  for (const auto& book : books) {
    JsonObject entry = bookArray.add<JsonObject>();
    entry["path"] = book.path;
    entry["title"] = book.title;
    entry["author"] = book.author;
    entry["seconds"] = book.seconds;
    if (book.bookId != 0) {
      char key[17];
      snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(book.bookId));
      entry["bookId"] = key;
    }
    if (book.lastReadAt != 0) entry["lastReadAt"] = book.lastReadAt;
    if (book.finishedAt != 0) entry["finishedAt"] = book.finishedAt;
    if (book.sessionCount != 0) entry["sessionCount"] = book.sessionCount;
    if (book.finished) entry["finished"] = true;
    entry["updatedAt"] = book.timestampDirty ? book.pendingUpdatedAt : book.updatedAt;
  }
  JsonArray dayArray = doc["days"].to<JsonArray>();
  for (const auto& day : days) {
    JsonObject entry = dayArray.add<JsonObject>();
    entry["date"] = day.date;
    entry["seconds"] = day.seconds;
  }
  JsonObject removed = doc["removedBooks"].to<JsonObject>();
  for (const auto& book : removedBooks) {
    char key[17];
    snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(book.bookId));
    removed[key] = book.updatedAt;
  }

  if (!yomuka::sync::writeJson(HISTORY_PATH, doc)) return false;
  for (auto& book : books) {
    if (book.timestampDirty) book.updatedAt = book.pendingUpdatedAt;
    book.timestampDirty = false;
  }
  loadStatus = yomuka::sync::ReadStatus::Present;
  dirty = false;
  LOG_DBG("RH", "Saved reading history: total=%lu seconds", static_cast<unsigned long>(totalSeconds));
  return true;
}

bool ReadingHistoryStore::loadFromFile() {
  JsonDocument doc;
  loadStatus = yomuka::sync::readJson(HISTORY_PATH, doc);
  if (loadStatus != yomuka::sync::ReadStatus::Present) return false;
  loadStatus = validateHistory(doc);
  if (loadStatus != yomuka::sync::ReadStatus::Present) return false;
  const uint8_t version = doc["version"] | 0;
  if (version != 1 && version != 2) {
    loadStatus = yomuka::sync::ReadStatus::Corrupt;
    return false;
  }
  totalSeconds = doc["totalSeconds"] | 0U;
  books.clear();
  for (JsonObject entry : doc["books"].as<JsonArray>()) {
    if (books.size() >= MAX_BOOKS) break;
    const std::string path = entry["path"] | std::string("");
    if (!path.empty()) {
      uint64_t bookId = 0;
      const char* storedBookId = entry["bookId"] | nullptr;
      if (storedBookId && strlen(storedBookId) == 16) {
        char* end = nullptr;
        bookId = static_cast<uint64_t>(strtoull(storedBookId, &end, 16));
        if (!end || *end != '\0') bookId = 0;
      }
      ReadingHistoryBook book{path, entry["title"] | std::string(""), entry["author"] | std::string(""),
                              entry["seconds"] | 0U, bookId};
      if (version >= 2) {
        book.lastReadAt = entry["lastReadAt"] | 0U;
        book.finishedAt = entry["finishedAt"] | 0U;
        book.sessionCount = entry["sessionCount"] | 0U;
        book.finished = entry["finished"] | (book.finishedAt != 0);
      }
      if (!yomuka::sync::readUpdateTime(entry, book.updatedAt)) {
        loadStatus = yomuka::sync::ReadStatus::Corrupt;
        return false;
      }
      books.push_back(std::move(book));
    }
  }
  days.clear();
  for (JsonObject entry : doc["days"].as<JsonArray>()) {
    if (days.size() >= MAX_DAYS) break;
    const uint32_t date = entry["date"] | 0U;
    if (date != 0) days.push_back({date, entry["seconds"] | 0U});
  }
  removedBooks.clear();
  for (JsonPairConst entry : doc["removedBooks"].as<JsonObjectConst>()) {
    uint64_t bookId = 0;
    if (!parseBookId(entry.key().c_str(), bookId)) return false;
    removedBooks.push_back({bookId, entry.value().as<uint32_t>()});
  }
  LOG_DBG("RH", "Loaded reading history: %u books, %u days", static_cast<unsigned>(books.size()),
          static_cast<unsigned>(days.size()));
  return true;
}
