#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sync/StorageIo.h"

struct ReadingHistoryBook {
  std::string path;
  std::string title;
  std::string author;
  uint32_t seconds = 0;
  uint64_t bookId = 0;      // EPUB archive fingerprint; zero for legacy/non-EPUB entries.
  uint32_t lastReadAt = 0;  // Unix time, or zero when the device clock is unavailable.
  uint32_t finishedAt = 0;  // Unix time, or zero when not recorded with a valid clock.
  uint32_t sessionCount = 0;
  bool finished = false;
  uint32_t updatedAt = 0;  // Last successfully persisted book-summary change.
  // Transient candidate date captured at mutation, not at a later retry/export.
  uint32_t pendingUpdatedAt = 0;
  bool timestampDirty = false;
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
  uint16_t finishedBookCount = 0;
  uint8_t topBookCount = 0;
  bool hasCalendarTime = false;
};

class ReadingHistoryStore {
  struct DayEntry {
    uint32_t date = 0;  // Local date in YYYYMMDD form.
    uint32_t seconds = 0;
  };
  struct RemovedBook {
    uint64_t bookId;
    uint32_t updatedAt;
  };

  static ReadingHistoryStore instance;
  std::vector<ReadingHistoryBook> books;
  std::vector<DayEntry> days;
  std::vector<RemovedBook> removedBooks;
  uint32_t totalSeconds = 0;
  std::string activePath;
  uint64_t activeBookId = 0;
  unsigned long lastTickMs = 0;
  unsigned long lastInteractionMs = 0;
  unsigned long lastSaveMs = 0;
  uint32_t pendingMilliseconds = 0;
  bool dirty = false;
  bool loaded = false;
  yomuka::sync::ReadStatus loadStatus = yomuka::sync::ReadStatus::Absent;

  void ensureLoaded();
  void addSeconds(uint32_t seconds);
  bool saveToFile();
  bool loadFromFile();
  uint32_t currentDate() const;
  uint32_t currentTimestamp() const;
  void noteBookChange(ReadingHistoryBook& book);

 public:
  // Decode persisted book summaries and deletion records without storage writes.
  static yomuka::sync::ReadStatus decodeSavedBooks(const JsonDocument& document,
                                                   std::vector<ReadingHistoryBook>& output,
                                                   std::vector<uint64_t>& removed);
  static ReadingHistoryStore& getInstance() { return instance; }

  // A session is deliberately separate from progress.bin.  It is used only by
  // readers and is periodically committed while the reader remains active.
  void beginSession(const std::string& path, const std::string& title, const std::string& author, uint64_t bookId = 0);
  void noteInteraction();
  void tick();
  bool endSession();
  // Commit pending elapsed time without ending an active session. A failed
  // save keeps dirty state and the previously committed updatedAt for retry.
  bool flushPending();
  yomuka::sync::ReadStatus readForSync(uint64_t bookId, ReadingHistoryBook& result, uint32_t& updatedAt);
  bool prepareForSync(const ReadingHistoryBook& book, uint32_t updatedAt, JsonDocument& output);
  bool reloadAfterSync();

  // Completion remains compatible with progress.bin, but the timestamp is
  // independent so history, library views, and a future sync layer do not
  // need to infer it from a page position.
  bool markFinished(const std::string& path, uint64_t bookId = 0);

  // Keep a book's accumulated time when the file browser moves it to Archive.
  bool moveBook(const std::string& oldPath, const std::string& newPath);
  // Keep accumulated reading time attached to a same-path EPUB update.
  // The previous archive data is retained elsewhere during the 0.7.x period,
  // while history records are rewritten to the current archive identity.
  bool migrateBookId(uint64_t previousBookId, uint64_t currentBookId);
  // Remove one book from book-level history and rankings. Aggregate time and
  // daily totals remain unchanged because older records are not book-attributed.
  bool removeBook(const std::string& path, uint64_t bookId = 0);
  // Remove all book-level and aggregate reading history.
  bool clearAll();
  ReadingHistorySummary getSummary();
  const std::vector<ReadingHistoryBook>& getBooks();
};

#define READING_HISTORY ReadingHistoryStore::getInstance()
