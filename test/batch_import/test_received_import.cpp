#include <iostream>

#include "import_boundary.h"
#include "sync/ProgressStorage.h"
using namespace yomuka::sync;
#define CHECK(x)                                   \
  do {                                             \
    if (!(x)) {                                    \
      std::cerr << __LINE__ << ": " << #x << "\n"; \
      return 1;                                    \
    }                                              \
  } while (0)
void epub(const std::string& path, const std::string& id) {
  Storage.put(path, id);
  Storage.put(path + "::META-INF/container.xml",
              "<container><rootfiles><rootfile media-type='application/oebps-package+xml' "
              "full-path='book.opf'/></rootfiles></container>");
  Storage.put(path + "::book.opf",
              "<package><manifest><item id='a' href='a.xhtml'/><item id='b' href='b.xhtml'/></manifest><spine><itemref "
              "idref='a'/><itemref idref='b'/></spine></package>");
}
int main() {
  const uint8_t raw[] = {0, 0, 0, 0, 10, 0, 0, 0};
  CHECK(saveProgress("/.crosspoint/books/0000000000000001/progress.bin", raw, 8));
  JsonDocument snapshot;
  CHECK(exportSnapshot({1, 2, "/a.epub", "A", ""}, Position, READING_HISTORY, snapshot) == ExchangeError::None);
  std::string json;
  serializeJson(snapshot, json);
  Storage.put("/YomukaSync/0000000000000001.json", json);
  epub("/a.epub", "1");
  ReceivedBook b;
  b.id = 1;
  b.path = "/a.epub";
  b.file = exchangeFilePath(1);
  b.status = ReceivedStatus::Matched;
  GfxRenderer renderer;
  auto before = Storage.snapshot();
  auto outcome = importReceivedSnapshot(b, renderer, [] { return true; });
  CHECK(outcome.result == ImportResult::Cancelled && Storage.snapshot() == before);
  layoutReady = false;
  outcome = importReceivedSnapshot(b, renderer, [] { return false; });
  CHECK(outcome.error == ImportError::Layout && !outcome.committed && Storage.snapshot() == before);
  layoutReady = true;
  outcome = importReceivedSnapshot(b, renderer, [] { return false; });
  CHECK(outcome.committed && outcome.result == ImportResult::Imported);
  Progress progress;
  uint32_t date = 0;
  CHECK(readProgress("/.crosspoint/books/0000000000000001/progress.bin", progress, date) == ReadStatus::Present &&
        progress.chapterPage == 2);
  CHECK(*Storage.files[b.file] == json);
  // Each import reloads shared history before preparing the next book.
  auto history = snapshot["units"]["history"];
  history["updatedAt"] = 0;
  auto data = history["data"];
  data["seconds"] = 99;
  data["sessionCount"] = 4;
  data["lastReadAt"] = 0;
  data["finishedAt"] = 0;
  data["finished"] = false;
  json.clear();
  serializeJson(snapshot, json);
  Storage.put(b.file, json);
  CHECK(importReceivedSnapshot(b, renderer, [] { return false; }).committed);
  ReceivedBook second = b;
  second.id = 2;
  second.path = "/b.epub";
  second.file = exchangeFilePath(2);
  epub(second.path, "2");
  snapshot["bookId"] = "0000000000000002";
  json.clear();
  serializeJson(snapshot, json);
  Storage.put(second.file, json);
  CHECK(importReceivedSnapshot(second, renderer, [] { return false; }).committed);
  ReadingHistoryBook firstHistory, secondHistory;
  CHECK(READING_HISTORY.readForSync(1, firstHistory, date) == ReadStatus::Present);
  CHECK(READING_HISTORY.readForSync(2, secondHistory, date) == ReadStatus::Present);
  CHECK(firstHistory.seconds == 99 && secondHistory.seconds == 99);
  snapshot["bookId"] = "0000000000000001";
  before = Storage.snapshot();
  Storage.put("/a.epub", "2");
  outcome = importReceivedSnapshot(b, renderer, [] { return false; });
  CHECK(outcome.error == ImportError::Source && !outcome.committed);
  Storage.restore(before);
  Storage.put(b.file, "bad");
  before = Storage.snapshot();
  outcome = importReceivedSnapshot(b, renderer, [] { return false; });
  CHECK(outcome.error == ImportError::File && Storage.snapshot() == before);
  Storage.restore(before);  // This before still contains malformed JSON; replace it below.
  snapshot["units"]["readerSettings"]["updatedAt"] = 0;
  snapshot["units"]["readerSettings"]["data"]["vertical"]["font"]["family"] = "noto-sans";
  snapshot["units"]["readerSettings"]["data"]["vertical"]["font"]["sdFamilyName"] = "MissingFont";
  json.clear();
  serializeJson(snapshot, json);
  Storage.put(b.file, json);
  fontsReady = false;
  before = Storage.snapshot();
  outcome = importReceivedSnapshot(b, renderer, [] { return false; });
  CHECK(outcome.error == ImportError::Font && Storage.snapshot() == before);
  fontsReady = true;
  settingsReady = false;
  outcome = importReceivedSnapshot(b, renderer, [] { return false; });
  CHECK(outcome.result == ImportResult::RestartRequired && !outcome.committed);
  std::cout << "PASS: actual import orchestration and storage transaction, cancellation/layout failure unchanged, "
               "projection applied, snapshot retained, replaced EPUB rejected, malformed JSON rejected\n";
}
