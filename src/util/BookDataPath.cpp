#include "BookDataPath.h"

#include <HalStorage.h>

#include <cstdio>

namespace BookDataPath {
namespace {

constexpr char kBookDataRoot[] = "/.crosspoint/books";

}  // namespace

std::string getDirectory(const uint64_t bookId) {
  char key[17];
  snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(bookId));
  return std::string(kBookDataRoot) + "/" + key;
}

std::string getProgressPath(const uint64_t bookId) { return getDirectory(bookId) + "/progress.bin"; }

std::string getBookmarkPath(const uint64_t bookId) { return getDirectory(bookId) + "/bookmarks.json"; }

bool ensureDirectory(const uint64_t bookId) {
  return Storage.ensureDirectoryExists(kBookDataRoot) && Storage.ensureDirectoryExists(getDirectory(bookId).c_str());
}

namespace {

bool copyIfMissing(const std::string& sourcePath, const std::string& destinationPath) {
  if (Storage.exists(destinationPath.c_str()) || !Storage.exists(sourcePath.c_str())) return true;
  const std::string temporaryPath = destinationPath + ".migrate.tmp";
  Storage.remove(temporaryPath.c_str());

  HalFile source;
  if (!Storage.openFileForRead("BID", sourcePath, source)) return false;
  {
    HalFile destination;
    if (!Storage.openFileForWrite("BID", temporaryPath, destination)) return false;
    uint8_t buffer[256];
    while (true) {
      const int bytesRead = source.read(buffer, sizeof(buffer));
      if (bytesRead < 0) {
        Storage.remove(temporaryPath.c_str());
        return false;
      }
      if (bytesRead == 0) break;
      if (destination.write(buffer, bytesRead) != static_cast<size_t>(bytesRead)) {
        Storage.remove(temporaryPath.c_str());
        return false;
      }
    }
    destination.flush();
  }
  source.close();
  if (!Storage.rename(temporaryPath.c_str(), destinationPath.c_str())) {
    Storage.remove(temporaryPath.c_str());
    return false;
  }
  return true;
}

}  // namespace

bool migrateArchiveData(const uint64_t previousBookId, const uint64_t currentBookId) {
  if (previousBookId == currentBookId) return true;
  const std::string previousDirectory = getDirectory(previousBookId);
  const std::string currentDirectory = getDirectory(currentBookId);
  const bool hasProgress = Storage.exists((previousDirectory + "/progress.bin").c_str());
  const bool hasBookmarks = Storage.exists((previousDirectory + "/bookmarks.json").c_str());
  if (!hasProgress && !hasBookmarks) return true;
  return ensureDirectory(currentBookId) &&
         copyIfMissing(previousDirectory + "/progress.bin", currentDirectory + "/progress.bin") &&
         copyIfMissing(previousDirectory + "/bookmarks.json", currentDirectory + "/bookmarks.json");
}

}  // namespace BookDataPath
