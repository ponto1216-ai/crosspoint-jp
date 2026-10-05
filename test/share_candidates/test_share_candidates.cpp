#include <HalStorage.h>

#include <cstdio>
#include <iostream>

#include "sync/ShareCandidateScanner.h"
#include "util/BookDataPath.h"

using namespace yomuka::sync;
unsigned checks = 0;
#define CHECK(c)                                   \
  do {                                             \
    ++checks;                                      \
    if (!(c)) {                                    \
      std::cerr << __LINE__ << ": " << #c << '\n'; \
      return 1;                                    \
    }                                              \
  } while (0)
std::string key(uint64_t id) {
  char output[17];
  std::snprintf(output, sizeof(output), "%016llx", static_cast<unsigned long long>(id));
  return output;
}
void putJson(const char* path, const JsonDocument& document) {
  std::string bytes;
  serializeJson(document, bytes);
  Storage.put(path, bytes);
}
JsonDocument history() {
  JsonDocument doc;
  doc["version"] = 2;
  doc["totalSeconds"] = 0;
  doc["books"].to<JsonArray>();
  doc["days"].to<JsonArray>();
  return doc;
}
void historyBook(JsonDocument& doc, uint64_t id, const char* path, const char* title) {
  auto book = doc["books"].add<JsonObject>();
  book["bookId"] = key(id);
  book["path"] = path;
  book["title"] = title;
  book["author"] = "";
  book["seconds"] = 0;
}
bool finish(ShareCandidateScanner& scanner) {
  for (unsigned i = 0; i < 20000; ++i)
    if (scanner.step()) return true;
  return false;
}
const ShareCandidate* find(const ShareCandidateScanner& scanner, uint64_t id) {
  for (const auto& book : scanner.books())
    if (book.bookId == id) return &book;
  return nullptr;
}
int main() {
  // Ordering survives unknown clocks; tombstones and zero-second sessions count.
  auto doc = history();
  historyBook(doc, 2, "/old.epub", "Second");
  historyBook(doc, 1, "/first.epub", "First");
  historyBook(doc, 0, "/legacy.epub", "Legacy");
  doc["removedBooks"][key(3)] = 0;
  putJson("/.crosspoint/reading-history.json", doc);
  JsonDocument settings;
  settings["formatVersion"] = 1;
  settings["books"][key(4)]["updatedAt"] = 0;     // empty override is a reset record
  settings["books"][key(5)]["writingMode"] = 99;  // corrupt one, retain the others
  putJson("/.crosspoint/book-reader-settings.json", settings);
  Storage.put("/first.epub", key(1));
  Storage.put("/moved/second.epub", key(2));
  Storage.put("/old.epub", key(99));  // same path contains another archive
  Storage.put("/Archived/deleted-history.epub", key(3));
  Storage.put("/reset.epub", key(4));
  Storage.put("/bad-setting.epub", key(5));
  Storage.put("/unread.epub", key(99));  // has no saved data
  Storage.put("/.crosspoint/books/0000000000000006/bookmarks.json", "{\"bookmarks\":[],\"updatedAt\":0}");
  Storage.put("/cleared-bookmarks.epub", key(6));
  Storage.put("/.crosspoint/books/0000000000000007/progress.bin", "bad");
  Storage.put("/bad-progress.epub", key(7));
  Storage.put("/.crosspoint/books/0000000000000008/section.bin", "cache only");
  Storage.put("/cache-only.epub", key(8));
  Storage.put("/YomukaSync/0000000000000009.json", "received only");
  Storage.put("/received-only.epub", key(9));
  Storage.put("/.crosspoint/books/000000000000000a/progress.bin", std::string(4, '\0'));
  Storage.put("/.crosspoint/hidden.epub", key(10));  // hidden book must remain unresolved
  const auto before = Storage.snapshot();
  ShareCandidateScanner scanner;
  CHECK(finish(scanner));
  CHECK(Storage.snapshot() == before);  // scan has absolutely no writes/migration/recovery
  CHECK(scanner.books().size() == 7);
  CHECK(scanner.books()[0].bookId == 2 && scanner.books()[1].bookId == 1);
  CHECK(find(scanner, 2)->path == "/moved/second.epub");
  CHECK(find(scanner, 3)->emptyUnits == SavedHistory);
  CHECK(find(scanner, 4)->emptyUnits == SavedSettings);
  CHECK(find(scanner, 5)->errors == SavedSettings);
  CHECK(find(scanner, 6)->emptyUnits == SavedBookmarks);
  CHECK(find(scanner, 7)->errors == SavedPosition);
  CHECK(!find(scanner, 8) && !find(scanner, 9) && !find(scanner, 99));
  CHECK(scanner.legacyCount == 1 && scanner.missingBooks == 1);

  // No history, no locator and >200 books: recursive enumeration is not capped.
  Storage = FakeStorage{};
  for (uint64_t id = 1; id <= 500; ++id) {
    Storage.put(BookDataPath::getProgressPath(id), std::string(4, '\0'));
    Storage.put("/Books/" + key(id) + ".EPUB", key(id));
  }
  Storage.put("/Books/duplicate.epub", key(1));
  const auto largeBefore = Storage.snapshot();
  ShareCandidateScanner large;
  CHECK(finish(large));
  CHECK(large.books().size() == 500);
  CHECK(large.checkedFiles == 501);
  CHECK(large.warnings == 0 && large.missingBooks == 0);
  CHECK(Storage.snapshot() == largeBefore);
  for (const auto& book : large.books()) CHECK(book.resolved() && book.units == SavedPosition);
  {
    ShareCandidateScanner cancelled;
    for (unsigned i = 0; i < 20; ++i) CHECK(!cancelled.step());
  }
  CHECK(Storage.snapshot() == largeBefore);  // early destruction/cancellation changes no files

  // Locator-only records never create a candidate; changed paths get verified.
  Storage = FakeStorage{};
  Storage.put(BookDataPath::getProgressPath(1), std::string(4, '\0'));
  JsonDocument paths;
  paths["formatVersion"] = 1;
  paths["paths"]["/z.epub"] = key(1);
  paths["paths"]["/a.epub"] = key(1);
  paths["paths"]["/no-data.epub"] = key(2);
  putJson("/.crosspoint/book-identity-paths.json", paths);
  Storage.put("/a.epub", key(1));
  Storage.put("/z.epub", key(1));
  Storage.put("/no-data.epub", key(2));
  ShareCandidateScanner hinted;
  CHECK(finish(hinted));
  CHECK(hinted.books().size() == 1 && hinted.books()[0].path == "/a.epub");

  // Corrupt aggregate reports an incomplete result; never becomes a clean empty list.
  Storage = FakeStorage{};
  Storage.put("/.crosspoint/reading-history.json", "not JSON");
  Storage.put("/.crosspoint/book-reader-settings.json.bak", "do not repair me");
  const auto brokenBefore = Storage.snapshot();
  ShareCandidateScanner broken;
  CHECK(finish(broken) && broken.warnings == 2);
  CHECK(Storage.snapshot() == brokenBefore);
  Storage.online = false;
  ShareCandidateScanner offline;
  CHECK(offline.step() && offline.warnings == 1);

  // Bad progress timestamp and backup-only progress are visible, never auto-repaired.
  Storage = FakeStorage{};
  Storage.put(BookDataPath::getProgressPath(1), std::string(4, '\0'));
  Storage.put(BookDataPath::getProgressPath(1) + ".sync-time", "bad metadata");
  Storage.put(BookDataPath::getProgressPath(2) + ".bak", std::string(4, '\0'));
  Storage.put("/one.epub", key(1));
  Storage.put("/two.epub", key(2));
  const auto metadataBefore = Storage.snapshot();
  ShareCandidateScanner metadata;
  CHECK(finish(metadata) && metadata.books().size() == 2);
  CHECK(find(metadata, 1)->errors == SavedPosition && find(metadata, 2)->errors == SavedPosition);
  CHECK(Storage.snapshot() == metadataBefore);

  // A valid JSON prefix is not a valid whole file. An SD read failure is not absence.
  Storage = FakeStorage{};
  doc = history();
  putJson("/.crosspoint/reading-history.json", doc);
  *Storage.files["/.crosspoint/reading-history.json"] += "garbage";
  ShareCandidateScanner trailing;
  CHECK(finish(trailing) && trailing.warnings == 1);
  putJson("/.crosspoint/reading-history.json", doc);
  Storage.failRead = "/.crosspoint/reading-history.json";
  ShareCandidateScanner failedRead;
  CHECK(finish(failedRead) && failedRead.warnings == 1);

  // Extremely deep trees produce an explicit incomplete-search warning.
  Storage = FakeStorage{};
  Storage.put(BookDataPath::getProgressPath(1), std::string(4, '\0'));
  std::string deep;
  for (unsigned i = 0; i < 35; ++i) deep += "/directory";
  Storage.put(deep + "/one.epub", key(1));
  ShareCandidateScanner depth;
  CHECK(finish(depth) && depth.warnings == 1 && depth.missingBooks == 1);

  // Duplicate history IDs are a per-book error, not two send candidates.
  ShareCandidateCatalog catalog;
  doc = history();
  historyBook(doc, 1, "/a.epub", "A");
  historyBook(doc, 1, "/b.epub", "B");
  size_t legacy = 0;
  CHECK(catalog.collectHistory(doc, legacy));
  CHECK(catalog.books().size() == 1 && catalog.books()[0].errors == SavedHistory);
  catalog.resolve(1, "/b.epub", 1);
  catalog.resolve(1, "/a.epub", 0);
  catalog.finish();
  CHECK(catalog.books()[0].path == "/a.epub");
  uint64_t id = 123;
  CHECK(!ShareCandidateCatalog::parseBookId("0000000000000000", id) && id == 123);
  CHECK(!ShareCandidateCatalog::parseBookId("000000000000000G", id));
  CHECK(!ShareCandidateCatalog::parseBookId("1", id));
  std::cout << "PASS: " << checks
            << " checks (500 books, ordering, relocation, tombstones, corrupt data and no writes)\n";
}
