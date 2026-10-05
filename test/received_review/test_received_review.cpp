#include <HalStorage.h>

#include <iostream>

#include "sync/ProgressStorage.h"
#include "sync/ReceivedBookScanner.h"
#include "sync/SnapshotExchange.h"
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
bool run(ReceivedBookScanner& scan) {
  for (int i = 0; i < 3000; ++i)
    if (scan.step()) return true;
  return false;
}
int main() {
  // Match the HAL assertion: a default constructed handle cannot be closed.
  Storage.assertValidClose = true;
  ReadingHistoryStore history;
  const uint8_t raw[] = {0, 0, 0, 0, 10, 0, 0, 0};
  CHECK(saveProgress("/.crosspoint/books/0000000000000001/progress.bin", raw, 8));
  JsonDocument doc;
  CHECK(exportSnapshot({1, 2, "/a.epub", "A", ""}, Position, history, doc) == ExchangeError::None);
  std::string json;
  serializeJson(doc, json);
  Storage.put("/YomukaSync/0000000000000001.json", json);

  doc["bookId"] = "0000000000000002";
  json.clear();
  serializeJson(doc, json);
  Storage.put("/YomukaSync/0000000000000002.json", json);
  Storage.put("/YomukaSync/0000000000000003.json", "bad");
  doc["bookId"] = "0000000000000004";
  doc["units"]["progress"]["data"]["spineIndex"] = 9;
  json.clear();
  serializeJson(doc, json);
  Storage.put("/YomukaSync/0000000000000004.json", json);
  doc["bookId"] = "0000000000000005";
  doc["units"]["progress"]["data"]["spineIndex"] = 0;
  json.clear();
  serializeJson(doc, json);
  Storage.put("/YomukaSync/0000000000000005.json", json);
  Storage.put("/YomukaSync/0000000000000006.json", json);  // filename != BookId
  Storage.put("/YomukaSync/incomplete.json.tmp", "partial");
  Storage.put("/YomukaSync/0000000000000009.json.bak", json);
  epub("/Book/a.epub", "1");
  epub("/Book/z.epub", "1");
  epub("/Book/outside.epub", "4");
  epub("/Book/broken.epub", "5");
  Storage.put("/Book/broken.epub::book.opf", "<package>");
  epub("/.crosspoint/hidden.epub", "2");
  Storage.files.erase("/.crosspoint/books/0000000000000001/progress.bin");
  const auto before = Storage.snapshot();
  ReceivedBookScanner all;
  CHECK(run(all));
  CHECK(all.books().size() == 6);
  CHECK(all.count(ReceivedStatus::Matched) == 1 && all.count(ReceivedStatus::Missing) == 1 &&
        all.count(ReceivedStatus::Error) == 4);
  CHECK(all.books()[0].path == "/Book/a.epub" && all.books()[0].spines == 2 && all.books()[0].units == Position);
  CHECK(all.books()[3].error == ReceivedError::Range && all.books()[4].error == ReceivedError::Source);
  CHECK(Storage.snapshot() == before);
  ReceivedBookScanner session({"/YomukaSync/0000000000000001.json", "/YomukaSync/0000000000000001.json"});
  CHECK(run(session) && session.books().size() == 1 && session.books()[0].status == ReceivedStatus::Matched);
  CHECK(Storage.snapshot() == before);
  ReceivedBookScanner gone({"/YomukaSync/0000000000000009.json"});
  CHECK(run(gone));
  CHECK(gone.books()[0].error == ReceivedError::File);
  CHECK(Storage.exists("/YomukaSync/0000000000000009.json.bak") &&
        !Storage.exists("/YomukaSync/0000000000000009.json"));
  Storage.failEnumeration = "/Book";
  ReceivedBookScanner enumeration({"/YomukaSync/0000000000000001.json"});
  CHECK(run(enumeration) && enumeration.warnings > 0 && enumeration.books()[0].error == ReceivedError::Search);
  Storage.failEnumeration.clear();
  // A broken/unreadable EPUB makes missing classification uncertain, never a false 'book missing'.
  Storage.put("/Bad.epub", "0");
  ReceivedBookScanner incomplete({"/YomukaSync/0000000000000002.json"});
  CHECK(run(incomplete));
  CHECK(incomplete.books()[0].status == ReceivedStatus::Error && incomplete.books()[0].error == ReceivedError::Search);
  Storage.files.erase("/Bad.epub");
  // A same-path replacement must not match the previous BookId.
  Storage.put("/Book/a.epub", "8");
  Storage.put("/Book/z.epub", "8");
  ReceivedBookScanner replaced({"/YomukaSync/0000000000000001.json"});
  CHECK(run(replaced));
  CHECK(replaced.books()[0].status == ReceivedStatus::Missing);
  ReceivedBookScanner stopped;
  CHECK(stopped.step([] { return true; }) && stopped.done());
  Storage.online = false;
  ReceivedBookScanner offline;
  CHECK(offline.step() && offline.warnings == 1);
  Storage.online = true;
  std::cout << "PASS: unread books and duplicate paths, session scoping, missing and broken files, BookId mismatch, "
               "range validation, replacement, incomplete discovery, cancellation, read-only storage\n";
  return 0;
}
