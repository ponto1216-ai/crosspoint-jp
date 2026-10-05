#include <HalStorage.h>

#include <cassert>
#include <iostream>

#include "sync/BatchSnapshot.h"
#include "sync/BookmarkStorage.h"
#include "sync/ProgressStorage.h"
#include "sync/SnapshotBookProbe.h"
#undef assert
#define assert(condition)                                  \
  do {                                                     \
    if (!(condition)) {                                    \
      std::cerr << __LINE__ << ": " << #condition << "\n"; \
      return 1;                                            \
    }                                                      \
  } while (0)
using namespace yomuka::sync;
void put(const std::string& path, const std::string& text) {
  Storage.files[path] = std::make_shared<std::string>(text);
}
void epub(const std::string& path, const std::string& id) {
  put(path, id);
  put(path + "::META-INF/container.xml",
      "<c:container xmlns:c='urn:test'><c:rootfiles><c:rootfile media-type='application/oebps-package+xml' "
      "full-path='book.opf'/></c:rootfiles></c:container>");
  put(path + "::book.opf",
      "<p:package xmlns:p='urn:test'><p:manifest><p:item id='a' href='a.xhtml'/><p:item id='b' "
      "href='b.xhtml'/></p:manifest><p:spine><p:itemref idref='a'/><p:itemref idref='b' "
      "linear='no'/></p:spine></p:package>");
}
int main() {
  epub("/a.epub", "1");
  epub("/b.epub", "2");
  epub("/c.epub", "3");
  uint32_t count = 123;
  assert(probeSnapshotBook("/a.epub", 1, count) && count == 2);
  assert(!probeSnapshotBook("/a.epub", 2, count));
  const auto container = *Storage.files["/a.epub::META-INF/container.xml"];
  put("/a.epub::META-INF/container.xml",
      "<container><rootfiles><rootfile media-type='application/oebps-package+xml' full-path='book.opf'/><rootfile "
      "media-type='application/oebps-package+xml' full-path='other.opf'/></rootfiles></container>");
  put("/a.epub::other.opf",
      "<package><manifest><item id='only' href='one.xhtml'/></manifest><spine><itemref "
      "idref='only'/></spine></package>");
  assert(probeSnapshotBook("/a.epub", 1, count) && count == 1);  // Same last rootfile choice as reader.
  put("/a.epub::META-INF/container.xml", container);
  auto opf = *Storage.files["/a.epub::book.opf"];
  for (const auto& bad :
       {std::string("<package>"), std::string("<!DOCTYPE package [<!ENTITY a 'x'>]><package/>"),
        std::string("<package><manifest/><spine><itemref idref='missing'/></spine></package>"), opf + "garbage"}) {
    put("/a.epub::book.opf", bad);
    count = 123;
    assert(!probeSnapshotBook("/a.epub", 1, count) && count == 123);
  }
  put("/a.epub::book.opf", opf);
  uint8_t raw[] = {1, 0, 2, 0, 10, 0, 0, 50};
  for (const auto& id : {"0000000000000001", "0000000000000002", "0000000000000003"})
    assert(saveProgress(std::string("/.crosspoint/books/") + id + "/progress.bin", raw, 8));
  const auto readerFile = *Storage.files["/.crosspoint/books/0000000000000001/progress.bin"];
  std::vector<ShareCandidate> books;
  for (size_t i = 0; i < 3; ++i) {
    ShareCandidate book;
    book.bookId = i + 1;
    book.path = std::string("/") + char('a' + i) + ".epub";
    book.units = SavedPosition;
    book.locationPriority = 0;
    books.push_back(book);
  }
  ReadingHistoryStore history;
  BatchSnapshot batch;
  batch.start({1, 1, 1});
  batch.step(books, history);
  assert(batch.completed() == 1 && batch.results()[0].result == BatchResult::Saved);
  const auto saved = *Storage.files[exchangeFilePath(1)];
  put("/b.epub", "99");  // Identity changed after discovery.
  batch.step(books, history);
  batch.step(books, history);
  assert(batch.done() && batch.results()[1].error == BatchError::Source &&
         batch.results()[2].result == BatchResult::Saved);
  assert(*Storage.files[exchangeFilePath(1)] == saved &&
         *Storage.files["/.crosspoint/books/0000000000000001/progress.bin"] == readerFile);
  batch.start({1, 0, 1});
  batch.step(books, history);
  batch.cancel();
  assert(batch.done() && batch.results()[0].result == BatchResult::Saved &&
         batch.results()[1].result == BatchResult::Cancelled);
  const auto before = *Storage.files[exchangeFilePath(1)];
  Storage.failWrite = exchangeFilePath(1) + ".tmp";
  batch.start({1});
  batch.step(books, history);
  assert(batch.results()[0].error == BatchError::WriteFailure && *Storage.files[exchangeFilePath(1)] == before);
  Storage.failWrite.clear();
  books[0].errors = SavedPosition;
  batch.start({1});
  batch.step(books, history);
  assert(batch.results()[0].error == BatchError::Data && *Storage.files[exchangeFilePath(1)] == before);
  books[0].errors = 0;
  raw[0] = 2;  // Valid encoding, outside EPUB spine range.
  assert(saveProgress("/.crosspoint/books/0000000000000001/progress.bin", raw, 8));
  batch.start({1});
  batch.step(books, history);
  assert(batch.results()[0].error == BatchError::Data && *Storage.files[exchangeFilePath(1)] == before);
  batch.start({});
  assert(batch.done());
  raw[0] = 1;
  assert(saveProgress("/.crosspoint/books/0000000000000001/progress.bin", raw, 8));
  assert(saveBookmarks("/.crosspoint/books/0000000000000001/bookmarks.json", {}));
  BookReaderSettings::Override override;
  assert(BookReaderSettings::save(1, override));
  books[0].units = All;
  batch.start({1});
  batch.step(books, history);
  assert(batch.results()[0].result == BatchResult::Saved);
  JsonDocument output;
  assert(readSnapshotForBook(exchangeFilePath(1), {1, 2, "/a.epub", "", ""}, output) == ExchangeError::None);
  assert(output["units"].size() == 4 && output["units"]["bookmarks"]["data"].size() == 0 &&
         output["units"]["readerSettings"]["data"].size() == 0);
  const auto lastSaved = *Storage.files[exchangeFilePath(1)];
  unsigned polls = 0;
  batch.start({1, 0, 1});
  batch.step(books, history, [&] { return ++polls == 2; });  // During XML streaming.
  assert(batch.done() && batch.results()[0].result == BatchResult::Cancelled &&
         batch.results()[1].result == BatchResult::Cancelled && *Storage.files[exchangeFilePath(1)] == lastSaved);
  polls = 0;
  batch.start({1, 0, 1});
  batch.step(books, history, [&] { return ++polls == 5; });  // After export, before publication.
  assert(batch.done() && batch.results()[0].result == BatchResult::Cancelled &&
         *Storage.files[exchangeFilePath(1)] == lastSaved);
  polls = 0;
  batch.start({1, 0, 1});
  batch.step(books, history, [&] { return ++polls == 6; });  // Publication completed; retain success.
  assert(batch.done() && batch.results()[0].result == BatchResult::Saved &&
         batch.results()[1].result == BatchResult::Cancelled);
  std::cout << "PASS: real XML probe, sequential snapshots, source change, range checks, failure preservation and "
               "cancellation\n";
}
