#include "FileBrowserActivity.h"

#include <Arduino.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <variant>

#include "../util/ConfirmationActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ReadingStatusHelper.h"
#include "components/UITheme.h"
#include "components/CacheStatusIcon.h"
#include "fontIds.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr int CACHE_STATUS_ICON_RADIUS = 7;
constexpr char CACHE_STATUS_VALUE_SPACER[] = "    ";

CachedBookStatus toCachedBookStatus(const Epub::CacheGenerationStatus status) {
  switch (status) {
    case Epub::CacheGenerationStatus::Complete:
      return CachedBookStatus::Complete;
    case Epub::CacheGenerationStatus::Resumable:
      return CachedBookStatus::Resumable;
    case Epub::CacheGenerationStatus::NotGenerated:
      return CachedBookStatus::NotGenerated;
  }
  return CachedBookStatus::Unknown;
}

Epub::CacheGenerationStatus fromCachedBookStatus(const CachedBookStatus status) {
  switch (status) {
    case CachedBookStatus::Complete:
      return Epub::CacheGenerationStatus::Complete;
    case CachedBookStatus::Resumable:
      return Epub::CacheGenerationStatus::Resumable;
    case CachedBookStatus::NotGenerated:
    case CachedBookStatus::Unknown:
      return Epub::CacheGenerationStatus::NotGenerated;
  }
  return Epub::CacheGenerationStatus::NotGenerated;
}

}  // namespace

void sortFileList(std::vector<std::string>& strs) {
  // Split directories once instead of repeating the directory test for every
  // comparison made by std::sort.
  const auto firstFile =
      std::partition(begin(strs), end(strs), [](const std::string& entry) { return entry.back() == '/'; });
  const auto naturalLess = [](const std::string& str1, const std::string& str2) {
    // Start naive natural sort
    const char* s1 = str1.c_str();
    const char* s2 = str2.c_str();

    // Iterate while both strings have characters
    while (*s1 && *s2) {
      // Check if both are at the start of a number
      if (isdigit(static_cast<unsigned char>(*s1)) && isdigit(static_cast<unsigned char>(*s2))) {
        // Skip leading zeros and track them
        while (*s1 == '0') s1++;
        while (*s2 == '0') s2++;

        // Count digits to compare lengths first
        int len1 = 0, len2 = 0;
        while (isdigit(static_cast<unsigned char>(s1[len1]))) len1++;
        while (isdigit(static_cast<unsigned char>(s2[len2]))) len2++;

        // Different length so return smaller integer value
        if (len1 != len2) return len1 < len2;

        // Same length so compare digit by digit
        for (int i = 0; i < len1; i++) {
          if (s1[i] != s2[i]) return s1[i] < s2[i];
        }

        // Numbers equal so advance pointers
        s1 += len1;
        s2 += len2;
      } else {
        // Regular case-insensitive character comparison
        char c1 = tolower(static_cast<unsigned char>(*s1));
        char c2 = tolower(static_cast<unsigned char>(*s2));
        if (c1 != c2) return c1 < c2;
        s1++;
        s2++;
      }
    }

    // One string is prefix of other
    return *s1 == '\0' && *s2 != '\0';
  };

  std::sort(begin(strs), firstFile, naturalLess);
  std::sort(firstFile, end(strs), naturalLess);
}

void FileBrowserActivity::cacheCurrentDirectory() {
  if (loadedPath.empty()) return;

  directoryCache.erase(
      std::remove_if(directoryCache.begin(), directoryCache.end(),
                     [this](const DirectoryCacheEntry& entry) { return entry.path == loadedPath; }),
      directoryCache.end());
  if (directoryCache.size() >= DIRECTORY_CACHE_SIZE) {
    directoryCache.erase(directoryCache.begin());
  }
  directoryCache.push_back(
      {std::move(loadedPath), std::move(files), std::move(fileStatuses), std::move(readingStatusCacheEntries),
       std::move(readingStatusKnown), std::move(fileCacheStatuses), std::move(fileCacheStatusKnown)});
  loadedPath.clear();
}

bool FileBrowserActivity::restoreCachedDirectory() {
  const auto cached = std::find_if(directoryCache.begin(), directoryCache.end(),
                                   [this](const DirectoryCacheEntry& entry) { return entry.path == basepath; });
  if (cached == directoryCache.end()) return false;

  loadedPath = std::move(cached->path);
  files = std::move(cached->files);
  fileStatuses = std::move(cached->statuses);
  readingStatusCacheEntries = std::move(cached->readingStatusCacheEntries);
  readingStatusKnown = std::move(cached->readingStatusKnown);
  fileCacheStatuses = std::move(cached->cacheStatuses);
  fileCacheStatusKnown = std::move(cached->cacheStatusKnown);
  directoryCache.erase(cached);
  return true;
}

void FileBrowserActivity::invalidateDirectoryCache(const std::string& path) {
  directoryCache.erase(
      std::remove_if(directoryCache.begin(), directoryCache.end(),
                     [&path](const DirectoryCacheEntry& entry) {
                       return entry.path == path ||
                              (entry.path.size() > path.size() && entry.path.compare(0, path.size(), path) == 0 &&
                               entry.path[path.size()] == '/');
                     }),
      directoryCache.end());
  if (loadedPath == path ||
      (loadedPath.size() > path.size() && loadedPath.compare(0, path.size(), path) == 0 &&
       loadedPath[path.size()] == '/')) {
    loadedPath.clear();
    files.clear();
    fileStatuses.clear();
    readingStatusCacheEntries.clear();
    readingStatusKnown.clear();
    fileCacheStatuses.clear();
    fileCacheStatusKnown.clear();
  }
}

FileBrowserActivity::DirectoryLoadResult FileBrowserActivity::loadFiles(bool forceReload) {
  const unsigned long totalStartedAt = millis();
  if (forceReload) {
    invalidateDirectoryCache(basepath);
  } else if (loadedPath == basepath) {
    LOG_DBG("FBPERF", "path=%s cache=current entries=%lu total=0 ms", basepath.c_str(),
            static_cast<unsigned long>(files.size()));
    return DirectoryLoadResult::Loaded;
  }

  cacheCurrentDirectory();
  if (!forceReload && restoreCachedDirectory()) {
    LOG_DBG("FBPERF", "path=%s cache=hit entries=%lu total=%lu ms", basepath.c_str(),
            static_cast<unsigned long>(files.size()), millis() - totalStartedAt);
    return DirectoryLoadResult::Loaded;
  }

  files.clear();
  fileStatuses.clear();
  readingStatusCacheEntries.clear();
  readingStatusKnown.clear();
  fileCacheStatuses.clear();
  fileCacheStatusKnown.clear();

  uint32_t scannedEntries = 0;
  uint32_t getNameCalls = 0;
  uint32_t isDirectoryCalls = 1;  // root
  unsigned long openMs = 0;
  unsigned long scanMs = 0;
  {
    const unsigned long openStartedAt = millis();
    auto root = Storage.open(basepath.c_str());
    openMs = millis() - openStartedAt;
    if (!root) {
      LOG_DBG("FBPERF", "path=%s open=failed openMs=%lu total=%lu ms", basepath.c_str(), openMs,
              millis() - totalStartedAt);
      return DirectoryLoadResult::OpenFailed;
    }
    if (!root.isDirectory()) {
      LOG_DBG("FBPERF", "path=%s type=file openMs=%lu total=%lu ms", basepath.c_str(), openMs,
              millis() - totalStartedAt);
      return DirectoryLoadResult::NotDirectory;
    }

    const unsigned long scanStartedAt = millis();
    char name[500];
    for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
      ++scannedEntries;
      file.getName(name, sizeof(name));
      ++getNameCalls;
      if ((!SETTINGS.showHiddenFiles && name[0] == '.') || strcmp(name, "System Volume Information") == 0) {
        continue;
      }

      const bool isDirectory = file.isDirectory();
      ++isDirectoryCalls;
      if (isDirectory) {
        // Store original (NFD) directory name for path construction.
        // NFC normalization is done at display time only.
        files.emplace_back(std::string(name) + "/");
      } else {
        std::string_view filename{name};
        if (mode == Mode::PickFirmware) {
          if (FsHelpers::checkFileExtension(filename, ".bin")) {
            files.emplace_back(filename);
          }
        } else if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
            FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
            FsHelpers::hasBmpExtension(filename)) {
          // Store original (NFD) filename for path construction.
          // NFC normalization is done at display time only.
          files.emplace_back(filename);
        }
      }
    }
    scanMs = millis() - scanStartedAt;
  }

  const unsigned long sortStartedAt = millis();
  sortFileList(files);
  const unsigned long sortMs = millis() - sortStartedAt;

  const unsigned long readingStatusStartedAt = millis();
  getReadingStatusCacheEntries("/.crosspoint", readingStatusCacheEntries);
  fileStatuses.assign(files.size(), ReadingStatus::Unread);
  readingStatusKnown.assign(files.size(), false);
  std::string fullBase = basepath;
  if (fullBase.back() != '/') fullBase += '/';
  for (size_t index = 0; index < files.size(); ++index) {
    CachedBookStatus cacheStatus = CachedBookStatus::Unknown;
    if (getBookListStatusFromIndex(fullBase + files[index], readingStatusCacheEntries, bookListStatusIndex,
                                   fileStatuses[index], cacheStatus)) {
      readingStatusKnown[index] = true;
    }
  }
  const unsigned long readingStatusMs = millis() - readingStatusStartedAt;

  uint32_t epubEntries = 0;
  for (const auto& file : files) {
    if (FsHelpers::hasEpubExtension(file)) {
      ++epubEntries;
    }
  }
  fileCacheStatuses.assign(files.size(), Epub::CacheGenerationStatus::NotGenerated);
  fileCacheStatusKnown.assign(files.size(), false);

  // Restore known cache-status values after sizing the current directory's
  // vectors. Entries absent from the cache root intentionally remain unknown.
  for (size_t index = 0; index < files.size(); ++index) {
    const std::string filepath = fullBase + files[index];
    if (FsHelpers::hasEpubExtension(files[index]) && !hasBookCacheEntry(filepath, readingStatusCacheEntries)) {
      // A missing per-book cache directory cannot be resumable or complete.
      // The root scan already established this without another SD access.
      fileCacheStatusKnown[index] = true;
      continue;
    }
    ReadingStatus readingStatus;
    CachedBookStatus cacheStatus = CachedBookStatus::Unknown;
    if (getBookListStatusFromIndex(filepath, readingStatusCacheEntries, bookListStatusIndex,
                                   readingStatus, cacheStatus) && cacheStatus != CachedBookStatus::Unknown) {
      fileCacheStatuses[index] = fromCachedBookStatus(cacheStatus);
      fileCacheStatusKnown[index] = true;
    }
  }

  loadedPath = basepath;
  LOG_DBG("FBPERF",
          "path=%s cache=miss open=%lu scan=%lu sort=%lu readingStatus=%lu cacheStatus=deferred total=%lu ms raw=%lu visible=%lu "
          "openNext=%lu getName=%lu isDirectory=%lu close=%lu",
          basepath.c_str(), openMs, scanMs, sortMs, readingStatusMs, millis() - totalStartedAt,
          static_cast<unsigned long>(scannedEntries), static_cast<unsigned long>(files.size()),
          static_cast<unsigned long>(scannedEntries + 1), static_cast<unsigned long>(getNameCalls),
          static_cast<unsigned long>(isDirectoryCalls), static_cast<unsigned long>(scannedEntries + 2));
  LOG_DBG("FBPERF", "path=%s epubCacheStatus=deferred epubs=%lu metadata=0 cover=0", basepath.c_str(),
          static_cast<unsigned long>(epubEntries));
  return DirectoryLoadResult::Loaded;
}

void FileBrowserActivity::onEnter() {
  Activity::onEnter();

  loadBookListStatusIndex("/.crosspoint", bookListStatusIndex);

  selectorIndex = 0;
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  const DirectoryLoadResult result = loadFiles();
  if (result == DirectoryLoadResult::OpenFailed) {
    basepath = "/";
    loadFiles();
  } else if (result == DirectoryLoadResult::NotDirectory) {
    lockLongPressBack = mappedInput.isPressed(MappedInputManager::Button::Back);

    const std::string oldPath = basepath;
    basepath = FsHelpers::extractFolderPath(basepath);
    loadFiles();

    const auto pos = oldPath.find_last_of('/');
    const std::string fileName = oldPath.substr(pos + 1);
    selectorIndex = findEntry(fileName);
  }

  requestUpdate();
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  if (bookListStatusIndexDirty) saveBookListStatusIndex("/.crosspoint", bookListStatusIndex);
  files.clear();
  fileStatuses.clear();
  readingStatusCacheEntries.clear();
  readingStatusKnown.clear();
  fileCacheStatuses.clear();
  fileCacheStatusKnown.clear();
}

void FileBrowserActivity::clearFileMetadata(const std::string& fullPath) {
  // Only clear cache for .epub files
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub(fullPath, "/.crosspoint").clearCache();
    LOG_DBG("FileBrowser", "Cleared metadata cache for: %s", fullPath.c_str());
  }
}

void FileBrowserActivity::loop() {
  if (bookListStatusIndexDirty) {
    RenderLock lock(*this);
    if (saveBookListStatusIndex("/.crosspoint", bookListStatusIndex)) bookListStatusIndexDirty = false;
  }

  // Long press BACK (1s+) goes to root folder
  // but Long press BACK (1s+) from ReaderActivity sends us here with the MappedInput already set.
  // So ignore it the first time.
  if (mode == Mode::Books && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= GO_HOME_MS &&
      basepath != "/" && !lockLongPressBack) {
    {
      // render() reads basepath and the file/status vectors on the render
      // task. loadFiles() can replace those vectors, so keep the update atomic.
      RenderLock lock(*this);
      basepath = "/";
      loadFiles();
      selectorIndex = 0;
    }
    requestUpdate();
    return;
  }

  if (lockLongPressBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockLongPressBack = false;
    return;
  }

  const int pathReserved = renderer.getLineHeight(SMALL_FONT_ID) + UITheme::getInstance().getMetrics().verticalSpacing;
  const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, pathReserved);

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (lockNextConfirmRelease) {
      lockNextConfirmRelease = false;
      return;
    }
    if (files.empty()) return;

    const std::string& entry = files[selectorIndex];
    bool isDirectory = (entry.back() == '/');

    if (mode == Mode::PickFirmware && !isDirectory) {
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      ActivityResult result{FilePathResult{cleanBasePath + entry}};
      setResult(std::move(result));
      finish();
      return;
    }

    if (mappedInput.getHeldTime() >= GO_HOME_MS) {
      // --- LONG PRESS ACTION: DELETE FILE/FOLDER ---
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      const std::string fullPath = cleanBasePath + (isDirectory ? entry.substr(0, entry.length() - 1) : entry);

      auto handler = [this, fullPath, isDirectory, entry](const ActivityResult& res) {
        if (!res.isCancelled) {
          // Right ボタン → 削除
          LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
          if (!isDirectory) clearFileMetadata(fullPath);
          const bool ok = isDirectory ? Storage.removeDir(fullPath.c_str()) : Storage.remove(fullPath.c_str());
          if (ok) {
            removeBookListStatusIndexEntry(fullPath, bookListStatusIndex);
            bookListStatusIndexDirty = true;
            LOG_DBG("FileBrowser", "Deleted successfully");
          } else {
            LOG_ERR("FileBrowser", "Failed to delete file: %s", fullPath.c_str());
            return;
          }
        } else if (std::holds_alternative<MenuResult>(res.data)) {
          const int code = std::get<MenuResult>(res.data).action;
          if (code == ConfirmationActivity::RESULT_NEVER) {
            // Left ボタン → アーカイブ（/Archived/ に移動）
            std::string filename = isDirectory ? entry.substr(0, entry.length() - 1) : entry;
            std::string destPath = "/Archived/" + filename;
            Storage.mkdir("/Archived");
            // 同名ファイルが存在する場合は先に削除
            if (Storage.exists(destPath.c_str())) {
              isDirectory ? Storage.removeDir(destPath.c_str()) : Storage.remove(destPath.c_str());
            }
            if (!isDirectory) clearFileMetadata(fullPath);
            if (Storage.rename(fullPath.c_str(), destPath.c_str())) {
              removeBookListStatusIndexEntry(fullPath, bookListStatusIndex);
              bookListStatusIndexDirty = true;
              invalidateDirectoryCache("/Archived");
              LOG_DBG("FileBrowser", "Archived to: %s", destPath.c_str());
            } else {
              LOG_ERR("FileBrowser", "Failed to archive: %s", fullPath.c_str());
              return;
            }
          } else if (code == ConfirmationActivity::RESULT_MIDDLE) {
            // Confirm ボタン → 既読にする
            if (isDirectory) return;
            if (!markAsFinished(fullPath, "/.crosspoint")) {
              LOG_ERR("FileBrowser", "Failed to mark as finished: %s", fullPath.c_str());
              return;
            }
            const CachedBookStatus cacheStatus =
                selectorIndex < fileCacheStatuses.size() && fileCacheStatusKnown[selectorIndex]
                    ? toCachedBookStatus(fileCacheStatuses[selectorIndex])
                    : CachedBookStatus::Unknown;
            updateBookListStatusIndex(fullPath, ReadingStatus::Finished, cacheStatus, bookListStatusIndex);
            bookListStatusIndexDirty = true;
          } else {
            // Back ボタン → キャンセル
            LOG_DBG("FileBrowser", "Action cancelled by user");
            return;
          }
        } else {
          // Back ボタン → キャンセル
          LOG_DBG("FileBrowser", "Action cancelled by user");
          return;
        }
        // 操作成功後、ファイル一覧を更新（アイコン状態反映のため）
        {
          // render() reads these values on the render task; loadFiles() may
          // free their current backing storage.
          RenderLock lock(*this);
          loadFiles(true);
          if (files.empty()) {
            selectorIndex = 0;
          } else if (selectorIndex >= files.size()) {
            selectorIndex = files.size() - 1;
          }
        }
        requestUpdate(true);
      };

      std::string heading = entry;

      // ディレクトリには既読操作を提供しない（btn2を空にする）
      const char* markAsReadLabel = isDirectory ? "" : tr(STR_MARK_AS_READ);
      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, "", tr(STR_ARCHIVE),
                                                                    tr(STR_DELETE), tr(STR_CANCEL), markAsReadLabel),
                             handler);
      return;
    } else {
      // --- SHORT PRESS ACTION: OPEN/NAVIGATE ---
      // render() reads basepath and the file/status vectors on the render
      // task. Mutate them only while it is excluded.
      RenderLock lock(*this);
      if (basepath.back() != '/') basepath += "/";

      if (isDirectory) {
        basepath += entry.substr(0, entry.length() - 1);
        loadFiles();
        selectorIndex = 0;
        lock.unlock();
        requestUpdate();
      } else {
        const std::string fullPath = basepath + entry;
        lock.unlock();  // Activity launch may acquire the render lock.
        onSelectBook(fullPath);
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;

        {
          // render() reads basepath and the file/status vectors on the render
          // task. loadFiles() can replace those vectors.
          RenderLock lock(*this);
          basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
          if (basepath.empty()) basepath = "/";
          loadFiles();

          const auto pos = oldPath.find_last_of('/');
          const std::string dirName = oldPath.substr(pos + 1) + "/";
          selectorIndex = findEntry(dirName);
        }

        requestUpdate();
      } else if (mode == Mode::PickFirmware) {
        ActivityResult result;
        result.isCancelled = true;
        setResult(std::move(result));
        finish();
      } else {
        onGoHome();
      }
    }
  }

  int listSize = static_cast<int>(files.size());
  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

std::string getFileName(std::string filename) {
  // NFC normalize for display (original NFD path is preserved in files[] for SD card access)
  utf8NfcNormalizeKana(filename);
  if (filename.back() == '/') {
    filename.pop_back();
    if (!UITheme::getInstance().getTheme().showsFileIcons()) {
      return "[" + filename + "]";
    }
    return filename;
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

std::string getFileExtension(std::string filename) {
  if (filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  return filename.substr(pos);
}

void FileBrowserActivity::render(RenderLock&&) {
  const unsigned long renderStartedAt = millis();
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName = (mode == Mode::PickFirmware)
                               ? std::string(tr(STR_SELECT_FIRMWARE_FILE))
                               : ((basepath == "/") ? std::string(tr(STR_SD_CARD))
                                                    : basepath.substr(basepath.rfind('/') + 1));
  utf8NfcNormalizeKana(folderName);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());
  const unsigned long headerMs = millis() - renderStartedAt;

  const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int pathReserved = pathLineHeight + metrics.verticalSpacing;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing - pathReserved;
  const bool showCacheStatusIcons = mode == Mode::Books && UITheme::getInstance().getTheme().showsFileIcons();
  uint32_t loadedReadingStatuses = 0;
  unsigned long readingStatusMs = 0;
  if (mode == Mode::Books && !files.empty()) {
    const int pageItems = std::max(1, contentHeight / metrics.listRowHeight);
    const int pageStart = (selectorIndex / pageItems) * pageItems;
    const int pageEnd = std::min(static_cast<int>(files.size()), pageStart + pageItems);
    const unsigned long readingStatusStartedAt = millis();
    std::string fullBase = basepath;
    if (fullBase.back() != '/') fullBase += '/';
    for (int index = pageStart; index < pageEnd; ++index) {
      if (readingStatusKnown[index]) continue;

      const std::string fullPath = fullBase + files[index];
      const bool hasCacheEntry =
          getReadingStatusFromCacheEntries(fullPath, "/.crosspoint", readingStatusCacheEntries, fileStatuses[index]);
      readingStatusKnown[index] = true;
      if (hasCacheEntry) {
        updateBookListStatusIndex(fullPath, fileStatuses[index], CachedBookStatus::Unknown, bookListStatusIndex);
        bookListStatusIndexDirty = true;
      }
      ++loadedReadingStatuses;
    }
    readingStatusMs = millis() - readingStatusStartedAt;
  }
  uint32_t loadedCacheStatuses = 0;
  unsigned long cacheStatusMs = 0;
  if (showCacheStatusIcons && !files.empty()) {
    const int pageItems = std::max(1, contentHeight / metrics.listRowHeight);
    const int pageStart = (selectorIndex / pageItems) * pageItems;
    const int pageEnd = std::min(static_cast<int>(files.size()), pageStart + pageItems);
    const unsigned long cacheStatusStartedAt = millis();
    for (int index = pageStart; index < pageEnd; ++index) {
      if (!FsHelpers::hasEpubExtension(files[index]) || fileCacheStatusKnown[index]) continue;

      std::string fullPath = basepath;
      if (fullPath.back() != '/') fullPath += '/';
      fullPath += files[index];
      fileCacheStatuses[index] = Epub(fullPath, "/.crosspoint").getCacheGenerationStatus();
      fileCacheStatusKnown[index] = true;
      updateBookListStatusIndex(fullPath, fileStatuses[index], toCachedBookStatus(fileCacheStatuses[index]),
                                bookListStatusIndex);
      bookListStatusIndexDirty = true;
      ++loadedCacheStatuses;
    }
    cacheStatusMs = millis() - cacheStatusStartedAt;
  }
  const unsigned long listStartedAt = millis();
  unsigned long filenameNormalizeUs = 0;
  if (files.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20,
                      mode == Mode::PickFirmware ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, files.size(), selectorIndex,
        [this, &filenameNormalizeUs](int index) {
          const unsigned long startedAt = micros();
          std::string filename = getFileName(files[index]);
          filenameNormalizeUs += micros() - startedAt;
          return filename;
        },
        nullptr,
        [this](int index) {
          return mode == Mode::PickFirmware ? UITheme::getFileIcon(files[index])
                                            : UITheme::getFileIcon(files[index], fileStatuses[index]);
        },
        [this, showCacheStatusIcons](int index) {
          if (mode == Mode::Books && FsHelpers::hasEpubExtension(files[index])) {
            // Reserve the rightmost space for the cache-status icon while
            // keeping the EPUB extension visible immediately to its left.
            if (showCacheStatusIcons) return getFileExtension(files[index]) + CACHE_STATUS_VALUE_SPACER;
          }
          return getFileExtension(files[index]);
        },
        false);

    if (showCacheStatusIcons) {
      const int rowHeight = metrics.listRowHeight;
      const int pageItems = contentHeight / rowHeight;
      const int pageStart = (selectorIndex / pageItems) * pageItems;
      // Keep the circle inside Lyra's rounded selection background and leave
      // a clear gap after the extension text.
      const int iconCenterX = pageWidth - metrics.contentSidePadding - CACHE_STATUS_ICON_RADIUS - 10;
      for (int index = pageStart; index < static_cast<int>(files.size()) && index < pageStart + pageItems; ++index) {
        if (!FsHelpers::hasEpubExtension(files[index])) continue;
        const int iconCenterY = contentTop + (index - pageStart) * rowHeight + rowHeight / 2;
        const bool ink = UITheme::getInstance().getTheme().showsFileIcons() || index != selectorIndex;
        CacheStatusIcon::draw(renderer, fileCacheStatuses[index], CACHE_STATUS_ICON_RADIUS, iconCenterX, iconCenterY,
                              ink);
      }
    }
  }
  const unsigned long listMs = millis() - listStartedAt;

  // Full path display
  const unsigned long footerStartedAt = millis();
  {
    const int pathY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - pathLineHeight;
    const int separatorY = pathY - metrics.verticalSpacing / 2;
    renderer.drawLine(0, separatorY, pageWidth - 1, separatorY, 3, true);
    const int pathMaxWidth = pageWidth - metrics.contentSidePadding * 2;
    // Left-truncate so the deepest directory is always visible
    const char* pathStr = basepath.c_str();
    const char* pathDisplay = pathStr;
    char leftTruncBuf[256];
    if (renderer.getTextWidth(SMALL_FONT_ID, pathStr) > pathMaxWidth) {
      const char ellipsis[] = "\xe2\x80\xa6";  // UTF-8 ellipsis (…)
      const int ellipsisWidth = renderer.getTextWidth(SMALL_FONT_ID, ellipsis);
      const int available = pathMaxWidth - ellipsisWidth;
      // Walk forward from the start until the suffix fits, skipping UTF-8 continuation bytes
      const char* p = pathStr;
      while (*p) {
        if (renderer.getTextWidth(SMALL_FONT_ID, p) <= available) break;
        ++p;
        while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
      }
      snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
      pathDisplay = leftTruncBuf;
    }
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, pathY, pathDisplay);
  }

  // Help text
  const bool selectingFirmwareFile = mode == Mode::PickFirmware && !files.empty() && files[selectorIndex].back() != '/';
  const auto labels =
      mappedInput.mapLabels(basepath == "/" ? (mode == Mode::PickFirmware ? tr(STR_BACK) : tr(STR_HOME)) : tr(STR_BACK),
                            files.empty() ? "" : (selectingFirmwareFile ? tr(STR_SELECT) : tr(STR_OPEN)),
                            files.empty() ? "" : tr(STR_DIR_UP), files.empty() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const unsigned long footerMs = millis() - footerStartedAt;
  const unsigned long displayStartedAt = millis();
  renderer.displayBuffer();
  LOG_DBG("FBPERF",
          "render path=%s header=%lu readingStatus=%lu ms loadedReadingStatuses=%lu cacheStatus=%lu ms "
          "loadedCacheStatuses=%lu list=%lu filenameNfc=%lu us footer=%lu display=%lu total=%lu ms",
          basepath.c_str(), headerMs, readingStatusMs, static_cast<unsigned long>(loadedReadingStatuses), cacheStatusMs,
          static_cast<unsigned long>(loadedCacheStatuses), listMs, filenameNormalizeUs, footerMs,
          millis() - displayStartedAt, millis() - renderStartedAt);
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
