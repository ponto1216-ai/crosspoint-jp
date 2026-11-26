#include <Arduino.h>
#include <FsHelpers.h>
#include <Txt.h>
#include <ZipFile.h>
#include <Logging.h>
#include <Epub/BookMetadataCache.h>
#include <cassert>
#include <cstdlib>
#include <memory>
#include <new>
#include <iostream>
struct CssParser { explicit CssParser(const std::string&) {} };
// Only the surrounding type is stubbed. All methods and metadata code are production sources.
class Epub {
 public:
  std::string filepath = "/book.txt";
  std::string cachePath = "/cache";
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  std::unique_ptr<CssParser> cssParser;
  bool isPlainText() const;
  bool loadPlainText(bool buildIfMissing);
  bool getItemSize(const std::string&, size_t*) const;
  bool readItemContentsToStream(const std::string&, Print&, size_t) const;
  uint8_t* readItemContentsToBytes(const std::string&, size_t*, bool) const;
};
#include "TxtEpubMethods.inc"
struct Output : Print {
  std::string text;
  size_t write(const uint8_t* data, size_t size) override {
    text.append(reinterpret_cast<const char*>(data), size);
    return size;
  }
};
int main() {
  Storage.files["/book.txt"] = std::string("\x93\xfa\x96\x7b\x8c\xea", 6);
  const auto original = Storage.files;
  Epub book;
  assert(!book.loadPlainText(false));
  assert(Storage.files == original);
  assert(book.loadPlainText(true));
  assert(ZipFile::openCalls == 0);
  assert(book.bookMetadataCache->getSpineCount() == 1);
  assert(book.bookMetadataCache->coreMetadata.title == "book");
  const auto html = Storage.files.at("/cache/text.xhtml");
  assert(html.find("日本語") != std::string::npos);
  assert(book.bookMetadataCache->getSpineEntry(0).cumulativeSize == html.size());
  Output output;
  assert(book.readItemContentsToStream("content.html", output, 1024));
  assert(output.text == html);
  size_t byteCount = 0;
  auto* bytes = book.readItemContentsToBytes("content.html", &byteCount, true);
  assert(bytes && byteCount == html.size() && bytes[byteCount] == 0);
  std::free(bytes);
  assert(!book.getItemSize("../book.txt", &byteCount));
  const auto writes = Storage.writes;
  assert(book.loadPlainText(false));
  assert(Storage.writes == writes);
  Storage.files["/cache/progress.bin"] = "new progress";
  Storage.files["/old-txt/progress.bin"] = "old progress";
  Storage.mkdir("/cache/sections");
  Storage.files["/cache/sections/0.bin"] = "old section";
  Storage.files["/book.txt"] = "ABCDEF";
  assert(!book.loadPlainText(false));
  assert(book.loadPlainText(true));
  assert(Storage.files.at("/cache/text.xhtml").find("ABCDEF") != std::string::npos);
  assert(!Storage.exists("/cache/sections/0.bin"));
  assert(Storage.files.at("/cache/progress.bin") == "new progress");
  assert(Storage.files.at("/old-txt/progress.bin") == "old progress");
  Storage.files["/book.txt"] = "GHIJKL";
  failWritePath = "/cache/text.xhtml.tmp";
  assert(!book.loadPlainText(true));
  failWritePath.clear();
  failRenamePath = "/cache/text.source";
  assert(!book.loadPlainText(true));
  assert(!book.loadPlainText(false));
  failRenamePath.clear();
  assert(book.loadPlainText(true));
  Storage.files["/cache/text.xhtml"].resize(10);
  assert(!book.loadPlainText(false));
  assert(book.loadPlainText(true));
  Storage.files["/cache/text.source"][4] = 0;
  assert(!book.loadPlainText(false));
  assert(book.loadPlainText(true));
  Storage.files["/cache/book.bin"] = "interrupted metadata";
  assert(!book.loadPlainText(false));
  assert(book.loadPlainText(true));
  failReadAt = 0;
  assert(!book.loadPlainText(true));
  failReadAt = std::numeric_limits<size_t>::max();
  assert(book.loadPlainText(true));
  Storage.mkdir("/zip");
  BookMetadataCache zip("/zip");
  assert(zip.beginWrite() && zip.beginContentOpfPass());
  zip.createSpineEntry("one.html");
  zip.createSpineEntry("two.html");
  assert(zip.endContentOpfPass() && zip.beginTocPass());
  zip.createTocEntry("One", "one.html", "", 0);
  assert(zip.endTocPass() && zip.endWrite());
  ZipFile::itemSizes = {{"one.html", 3}, {"two.html", 5}};
  assert(zip.buildBookBin("/book.epub", {}));
  assert(zip.load());
  assert(zip.getSpineEntry(0).cumulativeSize == 3);
  assert(zip.getSpineEntry(1).cumulativeSize == 8);
  assert(ZipFile::openCalls == 1);
  std::cout << "PASS: TXT EPUB integration, cache reuse, I/O recovery, and ZIP regression\n";
}
