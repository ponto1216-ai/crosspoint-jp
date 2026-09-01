#include "ReadingHistoryStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
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

void ReadingHistoryStore::beginSession(const std::string& path, const std::string& title, const std::string& author) {
  ensureLoaded();
  endSession();
  activePath = path;
  const auto it = std::find_if(books.begin(), books.end(),
                               [&path](const ReadingHistoryBook& entry) { return entry.path == path; });
  if (it == books.end()) {
    books.insert(books.begin(), {path, title, author, 0});
    if (books.size() > MAX_BOOKS) books.resize(MAX_BOOKS);
    dirty = true;
  } else {
    ReadingHistoryBook updated = *it;
    updated.title = title;
    updated.author = author;
    books.erase(it);
    books.insert(books.begin(), updated);
  }
  dirty = true;
  const unsigned long now = millis();
  lastTickMs = now;
  lastInteractionMs = now;
  lastSaveMs = now;
  pendingMilliseconds = 0;
  LOG_DBG("RH", "Started reading session: %s", path.c_str());
}

void ReadingHistoryStore::noteInteraction() {
  if (!activePath.empty()) lastInteractionMs = millis();
}

void ReadingHistoryStore::addSeconds(const uint32_t seconds) {
  if (seconds == 0 || activePath.empty()) return;
  totalSeconds += seconds;
  const auto it = std::find_if(books.begin(), books.end(),
                               [this](const ReadingHistoryBook& entry) { return entry.path == activePath; });
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

ReadingHistorySummary ReadingHistoryStore::getSummary() {
  ensureLoaded();
  tick();
  ReadingHistorySummary result;
  result.totalSeconds = totalSeconds;
  for (const auto& book : books) {
    if (book.seconds == 0) continue;
    ++result.bookCount;
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
  doc["version"] = 1;
  doc["totalSeconds"] = totalSeconds;
  JsonArray bookArray = doc["books"].to<JsonArray>();
  for (const auto& book : books) {
    JsonObject entry = bookArray.add<JsonObject>();
    entry["path"] = book.path;
    entry["title"] = book.title;
    entry["author"] = book.author;
    entry["seconds"] = book.seconds;
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
  if ((doc["version"] | 0) != 1) return false;
  totalSeconds = doc["totalSeconds"] | 0U;
  books.clear();
  for (JsonObject entry : doc["books"].as<JsonArray>()) {
    if (books.size() >= MAX_BOOKS) break;
    const std::string path = entry["path"] | std::string("");
    if (!path.empty()) books.push_back({path, entry["title"] | std::string(""), entry["author"] | std::string(""),
                                        entry["seconds"] | 0U});
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
