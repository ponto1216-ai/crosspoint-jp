#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct ReadingHistoryBook {
  std::string path;
  std::string title;
  std::string author;
  uint32_t seconds = 0;
};

struct ReadingHistorySummary {
  uint32_t totalSeconds = 0;
  uint32_t todaySeconds = 0;
  uint32_t weekSeconds = 0;
  uint32_t monthSeconds = 0;
  std::array<uint32_t, 7> recentDaySeconds = {};
  std::array<uint8_t, 7> recentDayWeekdays = {};
  std::array<ReadingHistoryBook, 3> topBooks = {};
  uint16_t bookCount = 0;
  uint8_t topBookCount = 0;
  bool hasCalendarTime = false;
};

class ReadingHistoryStore {
  struct DayEntry {
    uint32_t date = 0;  // Local date in YYYYMMDD form.
    uint32_t seconds = 0;
  };

  static ReadingHistoryStore instance;
  std::vector<ReadingHistoryBook> books;
  std::vector<DayEntry> days;
  uint32_t totalSeconds = 0;
  std::string activePath;
  unsigned long lastTickMs = 0;
  unsigned long lastInteractionMs = 0;
  unsigned long lastSaveMs = 0;
  uint32_t pendingMilliseconds = 0;
  bool dirty = false;
  bool loaded = false;

  void ensureLoaded();
  void addSeconds(uint32_t seconds);
  bool saveToFile();
  bool loadFromFile();
  uint32_t currentDate() const;

 public:
  static ReadingHistoryStore& getInstance() { return instance; }

  // A session is deliberately separate from progress.bin.  It is used only by
  // readers and is periodically committed while the reader remains active.
  void beginSession(const std::string& path, const std::string& title, const std::string& author);
  void noteInteraction();
  void tick();
  void endSession();

  // Keep a book's accumulated time when the file browser moves it to Archive.
  void moveBook(const std::string& oldPath, const std::string& newPath);
  ReadingHistorySummary getSummary();
  const std::vector<ReadingHistoryBook>& getBooks();
};

#define READING_HISTORY ReadingHistoryStore::getInstance()
