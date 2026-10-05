#include <algorithm>
#include <iostream>

#include "sync/ProgressStorage.h"
#include "sync/SnapshotComparison.h"
#include "sync/SnapshotExchange.h"
#include "sync/SnapshotPreview.h"
using namespace yomuka::sync;
#define CHECK(x)                                   \
  do {                                             \
    if (!(x)) {                                    \
      std::cerr << __LINE__ << ": " << #x << "\n"; \
      return 1;                                    \
    }                                              \
  } while (0)
bool contains(const std::vector<std::string>& lines, const std::string& text) {
  return std::any_of(lines.begin(), lines.end(),
                     [&](const auto& line) { return line.find(text) != std::string::npos; });
}
int main() {
  CHECK(compareUpdateTimes(200, 100).find("古い") != std::string::npos);
  CHECK(compareUpdateTimes(100, 200).find("新しい") != std::string::npos);
  CHECK(compareUpdateTimes(100, 100).find("内容一致とは限りません") != std::string::npos);
  CHECK(compareUpdateTimes(0, 100).find("判定しません") != std::string::npos);
  CHECK(compareUpdateTimes(100, 0).find("判定しません") != std::string::npos);
  const std::string progress = "/.crosspoint/books/0000000000000001/progress.bin";
  uint8_t raw[] = {0, 0, 0, 0, 10, 0, 0, 10};
  CHECK(saveProgress(progress, raw, 8));
  ReadingHistoryStore history;
  JsonDocument snapshot;
  CHECK(exportSnapshot({1, 2, "/a.epub", "A", ""}, All, history, snapshot) == ExchangeError::None);
  snapshot["units"]["progress"]["updatedAt"] = 100;
  snapshot["units"]["progress"]["data"]["percent"] = 25;
  const auto absentPreview = previewUnit(0, JsonVariantConst{}, snapshot["units"]["progress"], 2, true);
  CHECK(contains(absentPreview, "読書位置: なし"));
  std::string json;
  serializeJson(snapshot, json);
  Storage.put(exchangeFilePath(1), json);
  ReceivedBook b;
  b.id = 1;
  b.spines = 2;
  b.title = "A";
  b.path = "/a.epub";
  b.file = exchangeFilePath(1);
  BookReaderSettings::Override override;
  override.fields = BookReaderSettings::WritingMode;
  override.writingMode = 1;
  JsonDocument savedSettings;
  CHECK(BookReaderSettings::prepareForSync(1, override, 200, savedSettings));
  std::string settingsJson;
  serializeJson(savedSettings, settingsJson);
  Storage.put("/.crosspoint/book-reader-settings.json", settingsJson);
  ReadingHistoryBook localHistory;
  localHistory.bookId = 1;
  localHistory.path = "/a.epub";
  localHistory.title = "A";
  localHistory.seconds = 120;
  localHistory.sessionCount = 1;
  JsonDocument savedHistory;
  CHECK(history.prepareForSync(localHistory, 200, savedHistory));
  std::string historyJson;
  serializeJson(savedHistory, historyJson);
  Storage.put("/.crosspoint/reading-history.json", historyJson);
  snapshot["units"]["readerSettings"]["updatedAt"] = 100;
  snapshot["units"]["history"]["updatedAt"] = 100;
  json.clear();
  serializeJson(snapshot, json);
  Storage.put(b.file, json);
  auto before = Storage.snapshot();
  auto lines = compareReceivedBook(b);
  CHECK(contains(lines, "端末: 章 1 / 頁 1 / 10%") && contains(lines, "受信: 章 1 / 頁 1 / 25%"));
  CHECK(contains(lines, "しおりをすべて削除") && contains(lines, "本の設定をすべて解除"));
  CHECK(contains(lines, "端末: 1項目") && contains(lines, "端末: 0時間2分0秒 / 1回"));
  CHECK(contains(lines, "受信の更新日時が古い"));
  CHECK(Storage.snapshot() == before);
  Storage.files.erase(progress);
  Storage.put(progress + ".bak", std::string(reinterpret_cast<const char*>(raw), 8));
  before = Storage.snapshot();
  lines = compareReceivedBook(b);
  CHECK(contains(lines, "端末データを比較できません") && Storage.snapshot() == before &&
        !Storage.exists(progress.c_str()));
  Storage.files.erase(progress + ".bak");
  before = Storage.snapshot();
  lines = compareReceivedBook(b);
  CHECK(contains(lines, "端末にこの種類のデータなし") && Storage.snapshot() == before);
  snapshot["units"].remove("bookmarks");
  json.clear();
  serializeJson(snapshot, json);
  Storage.put(b.file, json);
  lines = compareReceivedBook(b);
  CHECK(contains(lines, "受信にない種類・端末のデータを保持"));
  Storage.put(b.file, "bad");
  before = Storage.snapshot();
  lines = compareReceivedBook(b);
  CHECK(contains(lines, "受信ファイルを比較できません") && Storage.snapshot() == before);
  std::cout << "PASS: read-only comparison, progress values, date ordering/unknown/equal, empty replacement warning, "
               "absence vs unreadable, backup not recovered, missing unit retained, malformed input\n";
}
