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
constexpr char HISTORY_TMP_PATH[] = "/.crosspoint/reading-history.json.tmp";
constexpr char HISTORY_BAK_PATH[] = "/.crosspoint/reading-history.json.bak";
constexpr uint32_t MIN_VALID_UNIX_TIME = 1704067200;  // 2024-01-01
constexpr unsigned long INACTIVITY_TIMEOUT_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long SAVE_INTERVAL_MS = 60UL * 1000UL;
constexpr size_t MAX_BOOKS = 100;
constexpr size_t MAX_DAYS = 366;
}  // namespace

ReadingHistoryStore ReadingHistoryStore::instance;

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
  struct tm localTime {};
  localtime_r(&now, &localTime);
  return static_cast<uint32_t>((localTime.tm_year + 1900) * 10000 + (localTime.tm_mon + 1) * 100 + localTime.tm_mday);
}

uint32_t ReadingHistoryStore::currentTimestamp() const {
  const time_t now = time(nullptr);
  return now >= MIN_VALID_UNIX_TIME ? static_cast<uint32_t>(now) : 0;
}

void ReadingHistoryStore::beginSession(const std::string& path, const std::string& title, const std::string& author,
                                       const uint64_t bookId) {
  ensureLoaded();
  endSession();
  activePath = path;
  activeBookId = bookId;
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

void ReadingHistoryStore::markFinished(const std::string& path, const uint64_t bookId) {
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
  it->finished = true;
  if (nowTimestamp != 0 && it->finishedAt == 0) it->finishedAt = nowTimestamp;
  if (nowTimestamp != 0) it->lastReadAt = nowTimestamp;
  dirty = true;
  saveToFile();
}

void ReadingHistoryStore::noteInteraction() {
  if (!activePath.empty()) lastInteractionMs = millis();
}

void ReadingHistoryStore::addSeconds(const uint32_t seconds) {
  if (seconds == 0 || activePath.empty()) return;
  totalSeconds += seconds;
  const auto it = std::find_if(books.begin(), books.end(),
                               [this](const ReadingHistoryBook& entry) {
                                 return entry.path == activePath || (activeBookId != 0 && entry.bookId == activeBookId);
                               });
  if (it != books.end()) it->seconds += seconds;

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

void ReadingHistoryStore::endSession() {
  if (activePath.empty()) return;
  tick();
  if (dirty) saveToFile();
  LOG_DBG("RH", "Finished reading session: %s", activePath.c_str());
  activePath.clear();
  activeBookId = 0;
  pendingMilliseconds = 0;
}

void ReadingHistoryStore::moveBook(const std::string& oldPath, const std::string& newPath) {
  ensureLoaded();
  const auto it = std::find_if(books.begin(), books.end(),
                               [&oldPath](const ReadingHistoryBook& entry) { return entry.path == oldPath; });
  if (it == books.end()) return;
  it->path = newPath;
  if (activePath == oldPath) activePath = newPath;
  dirty = true;
  saveToFile();
}

void ReadingHistoryStore::migrateBookId(const uint64_t previousBookId, const uint64_t currentBookId) {
  if (previousBookId == 0 || currentBookId == 0 || previousBookId == currentBookId) return;
  ensureLoaded();

  bool changed = false;
  for (auto& book : books) {
    if (book.bookId != previousBookId) continue;
    book.bookId = currentBookId;
    changed = true;
  }
  if (activeBookId == previousBookId) activeBookId = currentBookId;
  if (!changed) return;

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
  }
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
    const auto it = std::find_if(days.begin(), days.end(),
                                 [date](const DayEntry& entry) { return entry.date == date; });
    return it == days.end() ? 0U : it->seconds;
  };
  struct tm localTime {};
  localtime_r(&now, &localTime);
  const uint32_t todayDate = static_cast<uint32_t>((localTime.tm_year + 1900) * 10000 + (localTime.tm_mon + 1) * 100 +
                                                   localTime.tm_mday);
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
  Storage.mkdir("/.crosspoint");
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
  }
  JsonArray dayArray = doc["days"].to<JsonArray>();
  for (const auto& day : days) {
    JsonObject entry = dayArray.add<JsonObject>();
    entry["date"] = day.date;
    entry["seconds"] = day.seconds;
  }

  Storage.remove(HISTORY_TMP_PATH);
  {
    FsFile file;
    if (!Storage.openFileForWrite("RH", HISTORY_TMP_PATH, file)) return false;
    if (serializeJson(doc, file) == 0) {
      file.close();
      Storage.remove(HISTORY_TMP_PATH);
      return false;
    }
    file.flush();
    file.close();
  }
  const bool hadOriginal = Storage.exists(HISTORY_PATH);
  if (hadOriginal) {
    Storage.remove(HISTORY_BAK_PATH);
    if (!Storage.rename(HISTORY_PATH, HISTORY_BAK_PATH)) return false;
  }
  if (!Storage.rename(HISTORY_TMP_PATH, HISTORY_PATH)) {
    if (hadOriginal) Storage.rename(HISTORY_BAK_PATH, HISTORY_PATH);
    return false;
  }
  if (hadOriginal) Storage.remove(HISTORY_BAK_PATH);
  dirty = false;
  LOG_DBG("RH", "Saved reading history: total=%lu seconds", static_cast<unsigned long>(totalSeconds));
  return true;
}

bool ReadingHistoryStore::loadFromFile() {
  if (!Storage.exists(HISTORY_PATH)) return false;
  const String json = Storage.readFile(HISTORY_PATH);
  if (json.isEmpty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  const uint8_t version = doc["version"] | 0;
  if (version != 1 && version != 2) return false;
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
      books.push_back(std::move(book));
    }
  }
  days.clear();
  for (JsonObject entry : doc["days"].as<JsonArray>()) {
    if (days.size() >= MAX_DAYS) break;
    const uint32_t date = entry["date"] | 0U;
    if (date != 0) days.push_back({date, entry["seconds"] | 0U});
  }
  LOG_DBG("RH", "Loaded reading history: %u books, %u days", static_cast<unsigned>(books.size()),
          static_cast<unsigned>(days.size()));
  return true;
}
