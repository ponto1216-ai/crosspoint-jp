#include <HalStorage.h>
#include <Txt.h>
#include <ReadingStatusHelper.h>
#include <cassert>
#include <functional>
#include <iostream>
#include <memory>

namespace yomuka::sync {
inline bool recoverFile(const std::string&) { return true; }
}
#include "TxtProgressMethods.inc"

namespace EpdFontFamily { constexpr int REGULAR = 0; }
struct BlankLineBook {
  bool plain = false;
  bool isPlainText() const { return plain; }
};
struct BlankLineBlock {
  int lines = 0;
  void addWord(const char*, int) { ++lines; }
};
class ChapterHtmlSlimParser {
 public:
  static constexpr int MAX_CONSECUTIVE_EXPLICIT_BLANK_LINES = 1;
  std::unique_ptr<BlankLineBlock> currentTextBlock = std::make_unique<BlankLineBlock>();
  std::shared_ptr<BlankLineBook> epub = std::make_shared<BlankLineBook>();
  int consecutiveExplicitBlankLines = 0;
  bool addExplicitBlankLine();
};
#include "TxtBlankLines.inc"

int main() {
  const std::string filepath = "/本.MD";
  const std::string hash = std::to_string(std::hash<std::string>{}(filepath));
  std::string entry = "txt_" + hash;
  bool epubFormat = false;
  const char legacy[] = {4, 0, 0, 0, 1, 100};
  Storage.files["/cache/" + entry + "/progress.bin"] = std::string(legacy, sizeof(legacy));
  resolveTextProgress(filepath, "/cache", entry, epubFormat);
  assert(entry == "txt_" + hash && !epubFormat);
  assert(readProgress("/cache/" + entry + "/progress.bin", epubFormat).status == ReadingStatus::Finished);

  // Byte 4 is chapter page-count data in the new record, rather than a finished flag.
  const char current[] = {0, 0, 3, 0, 1, 0, 0, 42};
  const std::string currentPath = "/cache/epub_" + hash + "/progress.bin";
  Storage.files[currentPath] = std::string(current, sizeof(current));
  resolveTextProgress(filepath, "/cache", entry, epubFormat);
  assert(entry == "epub_" + hash && epubFormat);
  auto progress = readProgress(currentPath, epubFormat);
  assert(progress.status == ReadingStatus::Reading && progress.percent == 42);
  Storage.files[currentPath][6] = 1;
  Storage.files[currentPath][7] = 100;
  progress = readProgress(currentPath, epubFormat);
  assert(progress.status == ReadingStatus::Finished && progress.percent == 100);
  assert(Storage.files.at("/cache/txt_" + hash + "/progress.bin") == std::string(legacy, sizeof(legacy)));
  ChapterHtmlSlimParser epubParser;
  assert(epubParser.addExplicitBlankLine());
  assert(!epubParser.addExplicitBlankLine());
  ChapterHtmlSlimParser textParser;
  textParser.epub->plain = true;
  assert(textParser.addExplicitBlankLine());
  assert(textParser.addExplicitBlankLine());
  assert(textParser.addExplicitBlankLine());
  assert(textParser.currentTextBlock->lines == 3);
  std::cout << "PASS: legacy TXT and new EPUB progress selection and flags\n";
}
