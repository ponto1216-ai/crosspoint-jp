#include "AozoraActivity.h"

#include <ArduinoJson.h>
#include <FontManager.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <ZipFile.h>

#include <cstring>

#include "Epub/parsers/ContainerParser.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/TlsHeapReclaim.h"

// --- 50音行定義 ---

struct KanaRow {
  StrId label;
  const char* apiParam;
};

static const KanaRow KANA_ROWS[] = {
    {StrId::STR_KANA_A, "ア"},  {StrId::STR_KANA_KA, "カ"}, {StrId::STR_KANA_SA, "サ"}, {StrId::STR_KANA_TA, "タ"},
    {StrId::STR_KANA_NA, "ナ"}, {StrId::STR_KANA_HA, "ハ"}, {StrId::STR_KANA_MA, "マ"}, {StrId::STR_KANA_YA, "ヤ"},
    {StrId::STR_KANA_RA, "ラ"}, {StrId::STR_KANA_WA, "ワ"},
};
static constexpr int KANA_ROW_COUNT = 10;

// 各行の個別文字（作品名検索の2段階目で使用）
static const char* KANA_CHARS[][5] = {
    {"あ", "い", "う", "え", "お"}, {"か", "き", "く", "け", "こ"}, {"さ", "し", "す", "せ", "そ"},
    {"た", "ち", "つ", "て", "と"}, {"な", "に", "ぬ", "ね", "の"}, {"は", "ひ", "ふ", "へ", "ほ"},
    {"ま", "み", "む", "め", "も"}, {"や", "ゆ", "よ", "", ""},     {"ら", "り", "る", "れ", "ろ"},
    {"わ", "を", "ん", "", ""},
};
// 各行の文字数
static const int KANA_CHAR_COUNTS[] = {5, 5, 5, 5, 5, 5, 5, 3, 5, 3};

// --- ジャンル定義 ---

struct GenreRow {
  StrId label;
  const char* ndc;
};

static const GenreRow GENRES[] = {
    {StrId::STR_GENRE_NOVEL, "913"}, {StrId::STR_GENRE_POETRY, "911"},     {StrId::STR_GENRE_ESSAY, "914"},
    {StrId::STR_GENRE_DRAMA, "912"}, {StrId::STR_GENRE_FAIRY_TALE, "388"},
};
static constexpr int GENRE_COUNT = 5;

// --- トップメニュー項目数 ---
static constexpr int TOP_MENU_COUNT = 6;

// --- Constructor ---

AozoraActivity::AozoraActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Aozora", renderer, mappedInput) {}

// --- Lifecycle ---

void AozoraActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void AozoraActivity::onExit() {
  Activity::onExit();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);

  // Reload font caches that were freed for TLS memory
  FontManager::getInstance().loadSettings();
}

void AozoraActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  reclaimHeapForTls(renderer, "AOZORA");

  // Load download index
  indexManager_.loadAndPurge();
  favoritesManager_.load();

  {
    RenderLock lock(*this);
    state_ = TOP_MENU;
    selectedIndex_ = 0;
  }
}

// --- State navigation ---

void AozoraActivity::pushState(State newState) {
  stateStack_.push_back(state_);
  selectedIndexStack_.push_back(selectedIndex_);
  state_ = newState;
  selectedIndex_ = 0;
}

void AozoraActivity::popState() {
  if (stateStack_.empty()) {
    finish();
    return;
  }
  state_ = stateStack_.back();
  selectedIndex_ = selectedIndexStack_.back();
  stateStack_.pop_back();
  selectedIndexStack_.pop_back();
}

// --- URL encoding helper ---

static void urlEncodeUtf8(const char* src, char* dest, size_t destSize) {
  size_t pos = 0;
  for (size_t i = 0; src[i] && pos < destSize - 4; i++) {
    unsigned char c = static_cast<unsigned char>(src[i]);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      dest[pos++] = static_cast<char>(c);
    } else {
      pos += snprintf(dest + pos, destSize - pos, "%%%02X", c);
    }
  }
  dest[pos] = '\0';
}

// --- API calls (download JSON to SD temp file, then parse) ---

static constexpr const char* API_TMP_FILE = "/aozora_api.tmp";

namespace {

class SpineTagProbe final : public Print {
  char tagName[16] = {};
  size_t tagNameLength = 0;
  bool insideTag = false;
  bool readingTagName = false;

  void finishTagName() {
    tagName[tagNameLength] = '\0';
    if (strcmp(tagName, "spine") == 0 || strcmp(tagName, "opf:spine") == 0) {
      foundSpine = true;
    }
    readingTagName = false;
  }

 public:
  bool foundSpine = false;

  size_t write(uint8_t byte) override {
    const char c = static_cast<char>(byte);
    if (!insideTag) {
      if (c == '<') {
        insideTag = true;
        readingTagName = true;
        tagNameLength = 0;
      }
      return 1;
    }

    if (c == '>') {
      if (readingTagName) finishTagName();
      insideTag = false;
      return 1;
    }

    if (!readingTagName) return 1;
    if (tagNameLength == 0 && (c == '/' || c == '!' || c == '?')) {
      readingTagName = false;
      return 1;
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/') {
      finishTagName();
      return 1;
    }
    if (tagNameLength < sizeof(tagName) - 1) {
      tagName[tagNameLength++] = c;
    } else {
      readingTagName = false;
    }
    return 1;
  }
};

bool validateDownloadedEpub(const char* path) {
  const std::string epubPath(path);
  ZipFile zip(epubPath);
  if (!zip.open()) {
    LOG_ERR("AOZORA", "Downloaded file is not a readable ZIP: %s", path);
    return false;
  }
  zip.close();

  constexpr const char* containerPath = "META-INF/container.xml";
  size_t containerSize = 0;
  if (!zip.getInflatedFileSize(containerPath, &containerSize) || containerSize == 0) {
    LOG_ERR("AOZORA", "Downloaded EPUB has no container.xml");
    return false;
  }

  ContainerParser container(containerSize);
  if (!container.setup() || !zip.readFileToStream(containerPath, container, 512) || container.fullPath.empty()) {
    LOG_ERR("AOZORA", "Downloaded EPUB has an invalid container.xml");
    return false;
  }

  size_t opfSize = 0;
  if (!zip.getInflatedFileSize(container.fullPath.c_str(), &opfSize) || opfSize == 0) {
    LOG_ERR("AOZORA", "Downloaded EPUB has no package document: %s", container.fullPath.c_str());
    return false;
  }

  SpineTagProbe spineProbe;
  if (!zip.readFileToStream(container.fullPath.c_str(), spineProbe, 512) || !spineProbe.foundSpine) {
    LOG_ERR("AOZORA", "Downloaded EPUB package has no spine");
    return false;
  }

  LOG_DBG("AOZORA", "Validated downloaded EPUB: container=%uB opf=%uB", static_cast<unsigned>(containerSize),
          static_cast<unsigned>(opfSize));
  return true;
}

}  // namespace

static std::string lastApiError_;

static constexpr int API_MAX_RETRIES = 3;
static constexpr int EPUB_DOWNLOAD_MAX_RETRIES = 3;

static bool isRetryableEpubDownloadFailure(const HttpDownloader::DownloadError result) {
  if (result != HttpDownloader::HTTP_ERROR) return false;

  const int httpCode = HttpDownloader::lastHttpCode;
  return httpCode <= 0 || httpCode == 408 || httpCode == 429 || httpCode >= 500;
}

static void releaseTransientFontCachesForTls(GfxRenderer& renderer) { reclaimHeapForTls(renderer, "AOZORA"); }

static HttpDownloader::DownloadError downloadEpubWithRetry(GfxRenderer& renderer, const std::string& url,
                                                           const std::string& tmpPath,
                                                           HttpDownloader::ProgressCallback progress) {
  HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
  for (int attempt = 0; attempt < EPUB_DOWNLOAD_MAX_RETRIES; ++attempt) {
    if (attempt > 0) {
      LOG_DBG("AOZORA", "Retrying EPUB download %d/%d", attempt + 1, EPUB_DOWNLOAD_MAX_RETRIES);
      delay(1000);
    }

    Storage.remove(tmpPath.c_str());
    releaseTransientFontCachesForTls(renderer);
    result = HttpDownloader::downloadToFile(url, tmpPath, progress, 30000);
    if (result == HttpDownloader::OK || !isRetryableEpubDownloadFailure(result)) break;

    LOG_ERR("AOZORA", "EPUB download attempt %d/%d failed: err=%d http=%d", attempt + 1, EPUB_DOWNLOAD_MAX_RETRIES,
            static_cast<int>(result), HttpDownloader::lastHttpCode);
  }
  return result;
}

static bool fetchApiJson(GfxRenderer& renderer, const char* url, JsonDocument& doc) {
  LOG_DBG("AOZORA", "API call: %s (heap=%d)", url, ESP.getFreeHeap());

  HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
  for (int attempt = 0; attempt < API_MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      LOG_DBG("AOZORA", "Retry %d/%d", attempt + 1, API_MAX_RETRIES);
      delay(1000);
    }
    releaseTransientFontCachesForTls(renderer);
    result = HttpDownloader::downloadToFile(url, API_TMP_FILE, nullptr, 30000);
    if (result == HttpDownloader::OK) break;
    LOG_ERR("AOZORA", "API fetch attempt %d failed: err=%d http=%d", attempt + 1, result, HttpDownloader::lastHttpCode);
    Storage.remove(API_TMP_FILE);
  }

  if (result != HttpDownloader::OK) {
    char buf[128];
    snprintf(buf, sizeof(buf), "err=%d http=%d heap=%dKB", static_cast<int>(result), HttpDownloader::lastHttpCode,
             ESP.getFreeHeap() / 1024);
    lastApiError_ = buf;
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("AOZORA", API_TMP_FILE, file)) {
    LOG_ERR("AOZORA", "Failed to open temp file");
    Storage.remove(API_TMP_FILE);
    return false;
  }

  DeserializationError err = deserializeJson(doc, file);
  file.close();
  Storage.remove(API_TMP_FILE);

  if (err) {
    LOG_ERR("AOZORA", "JSON parse error: %s", err.c_str());
    return false;
  }

  return true;
}

bool AozoraActivity::fetchAuthors(const char* kanaPrefix) {
  // 検索キーを保存（再取得用）
  snprintf(lastAuthorsKanaPrefix_, sizeof(lastAuthorsKanaPrefix_), "%s", kanaPrefix);

  // 不要なバッファを解放してヒープ確保
  works_.clear();
  works_.shrink_to_fit();

  char encoded[32];
  urlEncodeUtf8(kanaPrefix, encoded, sizeof(encoded));

  char url[192];
  snprintf(url, sizeof(url), "%s/api/authors?kana_prefix=%s", API_BASE, encoded);

  JsonDocument doc;
  if (!fetchApiJson(renderer, url, doc)) {
    errorMessage_ = lastApiError_;
    return false;
  }

  return parseAuthorsJson(doc);
}

bool AozoraActivity::fetchWorks(const char* queryParam) {
  // 不要なバッファを解放してヒープ確保（TLSバッファ用）
  authors_.clear();
  authors_.shrink_to_fit();

  // 新しいクエリの場合はオフセットをリセットし、クエリを保存
  if (queryParam) {
    snprintf(lastWorksQuery_, sizeof(lastWorksQuery_), "%s", queryParam);
    worksOffset_ = 0;
  }

  // URL構築（kana_prefix の値部分はURLエンコードが必要）
  char url[256];
  if (strncmp(lastWorksQuery_, "kana_prefix=", 12) == 0) {
    char encoded[32];
    urlEncodeUtf8(lastWorksQuery_ + 12, encoded, sizeof(encoded));
    snprintf(url, sizeof(url), "%s/api/works?kana_prefix=%s&offset=%d&limit=%d", API_BASE, encoded, worksOffset_,
             WORKS_PAGE_SIZE);
  } else {
    snprintf(url, sizeof(url), "%s/api/works?%s&offset=%d&limit=%d", API_BASE, lastWorksQuery_, worksOffset_,
             WORKS_PAGE_SIZE);
  }

  JsonDocument doc;
  if (!fetchApiJson(renderer, url, doc)) {
    errorMessage_ = lastApiError_;
    return false;
  }

  worksTotal_ = doc["total"] | 0;
  return parseWorksJson(doc);
}

bool AozoraActivity::parseAuthorsJson(JsonDocument& doc) {
  authors_.clear();
  JsonArray arr = doc["authors"].as<JsonArray>();
  if (arr.isNull()) {
    LOG_ERR("AOZORA", "No 'authors' array in response");
    errorMessage_ = "Invalid response";
    return false;
  }

  authors_.reserve(arr.size());
  for (JsonObject obj : arr) {
    AuthorEntry entry;
    entry.id = obj["id"] | 0;
    snprintf(entry.name, sizeof(entry.name), "%s", (obj["name"] | ""));
    snprintf(entry.kana, sizeof(entry.kana), "%s", (obj["kana"] | ""));
    entry.workCount = obj["work_count"] | 0;
    authors_.push_back(entry);
  }

  LOG_DBG("AOZORA", "Parsed %zu authors", authors_.size());
  return true;
}

bool AozoraActivity::parseWorksJson(JsonDocument& doc) {
  works_.clear();
  JsonArray arr = doc["works"].as<JsonArray>();
  if (arr.isNull()) {
    LOG_ERR("AOZORA", "No 'works' array in response");
    errorMessage_ = "Invalid response";
    return false;
  }

  works_.reserve(arr.size());
  for (JsonObject obj : arr) {
    WorkEntry entry;
    entry.id = obj["id"] | 0;
    snprintf(entry.title, sizeof(entry.title), "%s", (obj["title"] | ""));
    snprintf(entry.kana, sizeof(entry.kana), "%s", (obj["kana"] | ""));
    snprintf(entry.ndc, sizeof(entry.ndc), "%s", (obj["ndc"] | ""));
    snprintf(entry.author, sizeof(entry.author), "%s", (obj["author"] | ""));
    works_.push_back(entry);
  }

  LOG_DBG("AOZORA", "Parsed %zu works", works_.size());
  return true;
}

bool AozoraActivity::downloadBook() {
  char url[256];
  snprintf(url, sizeof(url), "%s/api/convert?work_id=%d", API_BASE, selectedWorkId_);

  char relPath[160];
  if (!AozoraIndexManager::makeRelativePath(selectedWorkId_, selectedWorkTitle_, selectedWorkAuthor_, relPath,
                                            sizeof(relPath))) {
    LOG_ERR("AOZORA", "Failed to make relative path");
    errorMessage_ = "Path error";
    return false;
  }
  char destPath[192];
  snprintf(destPath, sizeof(destPath), "%s/%s", AozoraIndexManager::AOZORA_DIR, relPath);
  char tmpPath[208];
  snprintf(tmpPath, sizeof(tmpPath), "%s.download.tmp", destPath);

  if (!AozoraIndexManager::ensureDirectory() || !AozoraIndexManager::ensureAuthorDirectory(selectedWorkAuthor_)) {
    LOG_ERR("AOZORA", "Failed to create directory");
    errorMessage_ = "SD card error";
    return false;
  }

  // Never expose an incomplete EPUB as a downloaded book. A stale temporary
  // file can only come from an interrupted prior download and is safe to remove.
  downloadProgress_ = 0;
  downloadTotal_ = 0;
  auto result = downloadEpubWithRetry(renderer, url, tmpPath, [this](size_t downloaded, size_t total) {
    downloadProgress_ = downloaded;
    downloadTotal_ = total;
    requestUpdate(true);
  });

  if (result != HttpDownloader::OK) {
    LOG_ERR("AOZORA", "Download failed: err=%d http=%d", static_cast<int>(result), HttpDownloader::lastHttpCode);
    Storage.remove(tmpPath);
    char buf[80];
    snprintf(buf, sizeof(buf), "err=%d http=%d", static_cast<int>(result), HttpDownloader::lastHttpCode);
    errorMessage_ = buf;
    return false;
  }

  if (!validateDownloadedEpub(tmpPath)) {
    Storage.remove(tmpPath);
    errorMessage_ = "Invalid EPUB";
    return false;
  }

  // A pre-existing file belongs to a completed download. Preserve it rather
  // than replacing it from this new-download path.
  if (Storage.exists(destPath)) {
    LOG_ERR("AOZORA", "Download destination already exists: %s", destPath);
    Storage.remove(tmpPath);
    errorMessage_ = "File already exists";
    return false;
  }

  if (!Storage.rename(tmpPath, destPath)) {
    LOG_ERR("AOZORA", "Rename failed: %s -> %s", tmpPath, destPath);
    Storage.remove(tmpPath);
    errorMessage_ = "Rename failed";
    return false;
  }

  // Add to index
  if (!indexManager_.addEntry(selectedWorkId_, selectedWorkTitle_, selectedWorkAuthor_, relPath)) {
    LOG_ERR("AOZORA", "Failed to add index entry");
    // File is downloaded but index failed -- not critical
  } else {
    invalidateDownloadedPageCache();
  }

  LOG_DBG("AOZORA", "Downloaded: %s", destPath);
  return true;
}

bool AozoraActivity::updateBook() {
  char url[256];
  snprintf(url, sizeof(url), "%s/api/convert?work_id=%d", API_BASE, selectedWorkId_);

  char relPath[160];
  if (!AozoraIndexManager::makeRelativePath(selectedWorkId_, selectedWorkTitle_, selectedWorkAuthor_, relPath,
                                            sizeof(relPath))) {
    LOG_ERR("AOZORA", "Failed to make relative path");
    errorMessage_ = "Path error";
    return false;
  }
  char destPath[192];
  char tmpPath[208];
  char backupPath[224];
  snprintf(destPath, sizeof(destPath), "%s/%s", AozoraIndexManager::AOZORA_DIR, relPath);
  snprintf(tmpPath, sizeof(tmpPath), "%s.update.tmp", destPath);
  snprintf(backupPath, sizeof(backupPath), "%s%s", destPath, AozoraIndexManager::UPDATE_BACKUP_SUFFIX);

  if (!AozoraIndexManager::ensureDirectory() || !AozoraIndexManager::ensureAuthorDirectory(selectedWorkAuthor_)) {
    LOG_ERR("AOZORA", "Failed to create directory");
    errorMessage_ = "SD card error";
    return false;
  }

  // 既存のtmpファイルが残っていたら掃除
  downloadProgress_ = 0;
  downloadTotal_ = 0;
  // 一時ファイルにダウンロード（既存ファイルはこの時点では無傷）
  auto result = downloadEpubWithRetry(renderer, url, tmpPath, [this](size_t downloaded, size_t total) {
    downloadProgress_ = downloaded;
    downloadTotal_ = total;
    requestUpdate(true);
  });

  if (result != HttpDownloader::OK) {
    LOG_ERR("AOZORA", "Update download failed: err=%d http=%d", static_cast<int>(result), HttpDownloader::lastHttpCode);
    Storage.remove(tmpPath);  // 不完全な一時ファイルを削除、旧ファイルは保持
    char buf[80];
    snprintf(buf, sizeof(buf), "err=%d http=%d", static_cast<int>(result), HttpDownloader::lastHttpCode);
    errorMessage_ = buf;
    return false;
  }

  if (!validateDownloadedEpub(tmpPath)) {
    Storage.remove(tmpPath);
    errorMessage_ = "Invalid EPUB";
    return false;
  }

  // Preserve the previous book until the new file is in place. If the device
  // loses power after this rename, loadAndPurge() restores the backup.
  if (Storage.exists(backupPath)) {
    if (!Storage.exists(destPath)) {
      if (!Storage.rename(backupPath, destPath)) {
        LOG_ERR("AOZORA", "Failed to restore existing update backup: %s", backupPath);
        Storage.remove(tmpPath);
        errorMessage_ = "Backup restore failed";
        return false;
      }
    } else {
      Storage.remove(backupPath);
    }
  }
  if (Storage.exists(destPath) && !Storage.rename(destPath, backupPath)) {
    LOG_ERR("AOZORA", "Failed to preserve existing book: %s", destPath);
    Storage.remove(tmpPath);
    errorMessage_ = "Backup failed";
    return false;
  }

  if (!Storage.rename(tmpPath, destPath)) {
    LOG_ERR("AOZORA", "Rename failed: %s -> %s", tmpPath, destPath);
    Storage.remove(tmpPath);
    if (Storage.exists(backupPath) && Storage.rename(backupPath, destPath)) {
      errorMessage_ = "Update failed; original restored";
    } else {
      errorMessage_ = "Update rename failed";
    }
    return false;
  }

  Storage.remove(backupPath);

  LOG_DBG("AOZORA", "Updated: %s", destPath);
  return true;
}

// --- Input handling ---

void AozoraActivity::loop() {
  if (state_ == TOP_MENU) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    buttonNavigator_.onNextRelease([this] {
      if (selectedIndex_ < TOP_MENU_COUNT - 1) {
        selectedIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onPreviousRelease([this] {
      if (selectedIndex_ > 0) {
        selectedIndex_--;
        requestUpdate();
      }
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      switch (selectedIndex_) {
        case 0:  // お気に入り作家
        {
          RenderLock lock(*this);
          pushState(FAVORITE_AUTHORS);
        }
          requestUpdate();
          break;
        case 1:  // 作家から探す
          searchMode_ = SEARCH_AUTHOR;
          {
            RenderLock lock(*this);
            pushState(KANA_SELECT);
          }
          requestUpdate();
          break;
        case 2:  // 作品名から探す
          searchMode_ = SEARCH_TITLE;
          {
            RenderLock lock(*this);
            pushState(KANA_SELECT);
          }
          requestUpdate();
          break;
        case 3:  // ジャンルから探す
        {
          RenderLock lock(*this);
          pushState(GENRE_SELECT);
        }
          requestUpdate();
          break;
        case 4:  // 新着作品
        {
          {
            RenderLock lock(*this);
            pushState(LOADING);
          }
          requestUpdateAndWait();

          if (fetchWorks("sort=newest&limit=50")) {
            RenderLock lock(*this);
            state_ = WORK_LIST;
            selectedIndex_ = 0;
          } else {
            RenderLock lock(*this);
            state_ = ERROR;
          }
          requestUpdate();
        } break;
        case 5:  // ダウンロード済み
        {
          RenderLock lock(*this);
          pushState(DOWNLOADED_LIST);
        }
          requestUpdate();
          break;
      }
    }
  } else if (state_ == KANA_SELECT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    buttonNavigator_.onNextRelease([this] {
      if (selectedIndex_ < KANA_ROW_COUNT - 1) {
        selectedIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onPreviousRelease([this] {
      if (selectedIndex_ > 0) {
        selectedIndex_--;
        requestUpdate();
      }
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (searchMode_ == SEARCH_AUTHOR) {
        // 作家検索: 行全体でAPIを呼ぶ
        const char* kanaParam = KANA_ROWS[selectedIndex_].apiParam;
        {
          RenderLock lock(*this);
          pushState(LOADING);
        }
        requestUpdateAndWait();

        if (fetchAuthors(kanaParam)) {
          RenderLock lock(*this);
          state_ = AUTHOR_LIST;
          selectedIndex_ = 0;
        } else {
          RenderLock lock(*this);
          state_ = ERROR;
        }
        requestUpdate();
      } else {
        // 作品名検索: 個別文字選択画面へ
        selectedKanaRowIndex_ = selectedIndex_;
        {
          RenderLock lock(*this);
          pushState(KANA_CHAR_SELECT);
        }
        requestUpdate();
      }
    }
  } else if (state_ == KANA_CHAR_SELECT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    const int charCount = KANA_CHAR_COUNTS[selectedKanaRowIndex_];

    buttonNavigator_.onNextRelease([this, charCount] {
      if (selectedIndex_ < charCount - 1) {
        selectedIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onPreviousRelease([this] {
      if (selectedIndex_ > 0) {
        selectedIndex_--;
        requestUpdate();
      }
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      const char* kanaChar = KANA_CHARS[selectedKanaRowIndex_][selectedIndex_];

      {
        RenderLock lock(*this);
        pushState(LOADING);
      }
      requestUpdateAndWait();

      char query[64];
      snprintf(query, sizeof(query), "kana_prefix=%s", kanaChar);
      if (fetchWorks(query)) {
        RenderLock lock(*this);
        state_ = WORK_LIST;
        selectedIndex_ = 0;
      } else {
        RenderLock lock(*this);
        state_ = ERROR;
      }
      requestUpdate();
    }
  } else if (state_ == GENRE_SELECT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    buttonNavigator_.onNextRelease([this] {
      if (selectedIndex_ < GENRE_COUNT - 1) {
        selectedIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onPreviousRelease([this] {
      if (selectedIndex_ > 0) {
        selectedIndex_--;
        requestUpdate();
      }
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      const char* ndc = GENRES[selectedIndex_].ndc;

      {
        RenderLock lock(*this);
        pushState(LOADING);
      }
      requestUpdateAndWait();

      char query[64];
      snprintf(query, sizeof(query), "ndc=%s", ndc);
      if (fetchWorks(query)) {
        RenderLock lock(*this);
        state_ = WORK_LIST;
        selectedIndex_ = 0;
      } else {
        RenderLock lock(*this);
        state_ = ERROR;
      }
      requestUpdate();
    }
  } else if (state_ == AUTHOR_LIST) {
    // 作品一覧でメモリ解放された場合、作家一覧を再取得
    if (authors_.empty() && lastAuthorsKanaPrefix_[0]) {
      {
        RenderLock lock(*this);
        state_ = LOADING;
      }
      requestUpdateAndWait();
      if (fetchAuthors(lastAuthorsKanaPrefix_)) {
        RenderLock lock(*this);
        state_ = AUTHOR_LIST;
      } else {
        RenderLock lock(*this);
        state_ = ERROR;
      }
      requestUpdate();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    // Front buttons select an author one row at a time. Side buttons move one
    // visible list page, so a long author list stays quick to browse without
    // mixing cursor movement and page navigation on one press.
    buttonNavigator_.onRelease({MappedInputManager::Button::Right}, [this] {
      if (selectedIndex_ < static_cast<int>(authors_.size()) - 1) {
        selectedIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onRelease({MappedInputManager::Button::Left}, [this] {
      if (selectedIndex_ > 0) {
        selectedIndex_--;
        requestUpdate();
      }
    });

    const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      const int lastIndex = static_cast<int>(authors_.size()) - 1;
      const int nextIndex = std::min(lastIndex, selectedIndex_ + pageItems);
      if (nextIndex != selectedIndex_) {
        selectedIndex_ = nextIndex;
        requestUpdate();
      }
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      const int previousIndex = std::max(0, selectedIndex_ - pageItems);
      if (previousIndex != selectedIndex_) {
        selectedIndex_ = previousIndex;
        requestUpdate();
      }
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!authors_.empty()) {
        const auto& author = authors_[selectedIndex_];
        selectedAuthorId_ = author.id;
        snprintf(selectedAuthorName_, sizeof(selectedAuthorName_), "%s", author.name);
        {
          RenderLock lock(*this);
          pushState(AUTHOR_ACTION);
          actionMenuIndex_ = 0;
        }
        requestUpdate();
      }
    }
  } else if (state_ == WORK_LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    // Keep side buttons exclusively for page fetches. ButtonNavigator's
    // next/previous helpers also include Up/Down, which would otherwise move
    // the cursor and fetch a new page on the same side-button press.
    buttonNavigator_.onRelease({MappedInputManager::Button::Right}, [this] {
      if (selectedIndex_ < static_cast<int>(works_.size()) - 1) {
        selectedIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onRelease({MappedInputManager::Button::Left}, [this] {
      if (selectedIndex_ > 0) {
        selectedIndex_--;
        requestUpdate();
      }
    });

    // This horizontal list follows physical side-button order, irrespective of
    // the reader's configurable PageBack/PageForward preference.
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (worksOffset_ + WORKS_PAGE_SIZE < worksTotal_) {
        worksOffset_ += WORKS_PAGE_SIZE;
        {
          RenderLock lock(*this);
          state_ = LOADING;
        }
        requestUpdateAndWait();
        if (fetchWorks(nullptr)) {  // nullptr = 前回のクエリを再利用
          RenderLock lock(*this);
          state_ = WORK_LIST;
          selectedIndex_ = 0;
        } else {
          RenderLock lock(*this);
          state_ = ERROR;
        }
        requestUpdate();
      }
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (worksOffset_ > 0) {
        worksOffset_ = (worksOffset_ >= WORKS_PAGE_SIZE) ? worksOffset_ - WORKS_PAGE_SIZE : 0;
        {
          RenderLock lock(*this);
          state_ = LOADING;
        }
        requestUpdateAndWait();
        if (fetchWorks(nullptr)) {
          RenderLock lock(*this);
          state_ = WORK_LIST;
          selectedIndex_ = 0;
        } else {
          RenderLock lock(*this);
          state_ = ERROR;
        }
        requestUpdate();
      }
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!works_.empty()) {
        const auto& work = works_[selectedIndex_];
        selectedWorkId_ = work.id;
        snprintf(selectedWorkTitle_, sizeof(selectedWorkTitle_), "%s", work.title);
        snprintf(selectedWorkAuthor_, sizeof(selectedWorkAuthor_), "%s",
                 work.author[0] ? work.author : selectedAuthorName_);

        {
          RenderLock lock(*this);
          pushState(WORK_DETAIL);
        }
        requestUpdate();
      }
    }
  } else if (state_ == WORK_DETAIL) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    const bool alreadyDownloaded = indexManager_.isDownloaded(selectedWorkId_);

    if (!alreadyDownloaded) {
      // 未ダウンロード: Right = 取得
      if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
        {
          RenderLock lock(*this);
          state_ = DOWNLOADING;
          downloadProgress_ = 0;
          downloadTotal_ = 0;
        }
        requestUpdateAndWait();

        if (downloadBook()) {
          RenderLock lock(*this);
          state_ = WORK_DETAIL;  // Stay on detail, now showing "downloaded"
        } else {
          RenderLock lock(*this);
          state_ = ERROR;
        }
        requestUpdate();
      }
    } else {
      // ダウンロード済み: Left = 削除, Right = 更新
      if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
        if (indexManager_.removeEntry(selectedWorkId_)) {
          invalidateDownloadedPageCache();
          // 削除で selectedIndex_ が範囲外になり得る（DOWNLOADED_LIST 経由で来ている場合）
          const int newTotal = static_cast<int>(indexManager_.activeCount());
          if (selectedIndex_ >= newTotal && newTotal > 0) {
            selectedIndex_ = newTotal - 1;
          } else if (newTotal == 0) {
            selectedIndex_ = 0;
          }
          {
            RenderLock lock(*this);
            popState();
          }
          requestUpdate();
        }
      } else if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
        {
          RenderLock lock(*this);
          state_ = DOWNLOADING;
          downloadProgress_ = 0;
          downloadTotal_ = 0;
        }
        requestUpdateAndWait();

        if (updateBook()) {
          RenderLock lock(*this);
          state_ = WORK_DETAIL;  // 更新成功、詳細画面に戻る
        } else {
          RenderLock lock(*this);
          state_ = ERROR;
        }
        requestUpdate();
      }
    }
  } else if (state_ == FAVORITE_AUTHORS) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    const auto& favEntries = favoritesManager_.entries();

    if (!favEntries.empty()) {
      buttonNavigator_.onNextRelease([this, &favEntries] {
        if (selectedIndex_ < static_cast<int>(favEntries.size()) - 1) {
          selectedIndex_++;
          requestUpdate();
        }
      });

      buttonNavigator_.onPreviousRelease([this] {
        if (selectedIndex_ > 0) {
          selectedIndex_--;
          requestUpdate();
        }
      });

      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        const auto& fav = favEntries[selectedIndex_];
        selectedAuthorId_ = fav.authorId;
        snprintf(selectedAuthorName_, sizeof(selectedAuthorName_), "%s", fav.name);
        {
          RenderLock lock(*this);
          pushState(AUTHOR_ACTION);
          actionMenuIndex_ = 0;
        }
        requestUpdate();
      }
    }

  } else if (state_ == AUTHOR_ACTION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    buttonNavigator_.onNextRelease([this] {
      if (actionMenuIndex_ < 1) {
        actionMenuIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onPreviousRelease([this] {
      if (actionMenuIndex_ > 0) {
        actionMenuIndex_--;
        requestUpdate();
      }
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (actionMenuIndex_ == 0) {
        // 作品を見る
        {
          RenderLock lock(*this);
          state_ = LOADING;
        }
        requestUpdateAndWait();

        char query[64];
        snprintf(query, sizeof(query), "author_id=%d", selectedAuthorId_);
        if (fetchWorks(query)) {
          RenderLock lock(*this);
          state_ = WORK_LIST;
          selectedIndex_ = 0;
        } else {
          RenderLock lock(*this);
          state_ = ERROR;
        }
        requestUpdate();
      } else {
        // お気に入り追加/削除
        bool wasFavorited = favoritesManager_.isFavorited(selectedAuthorId_);
        if (wasFavorited) {
          favoritesManager_.removeAuthor(selectedAuthorId_);
        } else {
          const char* kana = "";
          for (const auto& a : authors_) {
            if (a.id == selectedAuthorId_) {
              kana = a.kana;
              break;
            }
          }
          favoritesManager_.addAuthor(selectedAuthorId_, selectedAuthorName_, kana);
        }
        {
          RenderLock lock(*this);
          popState();
        }
        requestUpdate();
      }
    }

  } else if (state_ == DOWNLOADED_LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
      return;
    }

    const int total = static_cast<int>(indexManager_.activeCount());

    buttonNavigator_.onNextRelease([this, total] {
      if (selectedIndex_ < total - 1) {
        selectedIndex_++;
        requestUpdate();
      }
    });

    buttonNavigator_.onPreviousRelease([this] {
      if (selectedIndex_ > 0) {
        selectedIndex_--;
        requestUpdate();
      }
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (total > 0 && selectedIndex_ < total) {
        // 選択中のエントリがページキャッシュ内にあることを保証してから読む
        const int pageStart = (selectedIndex_ / DL_PAGE_SIZE) * DL_PAGE_SIZE;
        if (dlPageStart_ != pageStart) {
          loadDownloadedPage(pageStart);
        }
        const int localIdx = selectedIndex_ - dlPageStart_;
        if (localIdx >= 0 && localIdx < dlPageCount_) {
          const auto& entry = dlPageCache_[localIdx];
          selectedWorkId_ = entry.workId;
          snprintf(selectedWorkTitle_, sizeof(selectedWorkTitle_), "%s", entry.title);
          snprintf(selectedWorkAuthor_, sizeof(selectedWorkAuthor_), "%s", entry.author);

          {
            RenderLock lock(*this);
            pushState(WORK_DETAIL);
          }
          requestUpdate();
        }
      }
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        popState();
      }
      requestUpdate();
    }
  }
}

// --- Rendering ---

void AozoraActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageHeight = renderer.getScreenHeight();
  const auto layout = UiLayout::from(renderer);
  const int contentLeft = layout.content.x + metrics.contentSidePadding;
  const int contentWidth = layout.content.width - metrics.contentSidePadding * 2;
  const int centerOffset = layout.content.x + layout.content.width / 2 - renderer.getScreenWidth() / 2;
  const int bottomHints = layout.landscape ? 0 : metrics.buttonHintsHeight + metrics.verticalSpacing;

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{layout.content.x, metrics.topPadding, layout.content.width, metrics.headerHeight},
                 tr(STR_AOZORA_BUNKO));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING) {
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, centerY, tr(STR_LOADING_LIST), true, centerOffset);

  } else if (state_ == TOP_MENU) {
    GUI.drawList(
        renderer, Rect{layout.content.x, contentTop, layout.content.width, pageHeight - contentTop - bottomHints},
        TOP_MENU_COUNT, selectedIndex_,
        [](int index) -> std::string {
          switch (index) {
            case 0:
              return tr(STR_FAVORITE_AUTHORS);
            case 1:
              return tr(STR_SEARCH_BY_AUTHOR);
            case 2:
              return tr(STR_SEARCH_BY_TITLE);
            case 3:
              return tr(STR_SEARCH_BY_GENRE);
            case 4:
              return tr(STR_NEWEST_WORKS);
            case 5:
              return tr(STR_DOWNLOADED_BOOKS);
            default:
              return "";
          }
        },
        nullptr, nullptr, nullptr, false, nullptr);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state_ == KANA_SELECT) {
    GUI.drawList(
        renderer, Rect{layout.content.x, contentTop, layout.content.width, pageHeight - contentTop - bottomHints},
        KANA_ROW_COUNT, selectedIndex_, [](int index) -> std::string { return I18N.get(KANA_ROWS[index].label); },
        nullptr, nullptr, nullptr, false, nullptr);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state_ == KANA_CHAR_SELECT) {
    const int charCount = KANA_CHAR_COUNTS[selectedKanaRowIndex_];
    GUI.drawList(
        renderer, Rect{layout.content.x, contentTop, layout.content.width, pageHeight - contentTop - bottomHints},
        charCount, selectedIndex_,
        [this](int index) -> std::string { return KANA_CHARS[selectedKanaRowIndex_][index]; }, nullptr, nullptr,
        nullptr, false, nullptr);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state_ == GENRE_SELECT) {
    GUI.drawList(
        renderer, Rect{layout.content.x, contentTop, layout.content.width, pageHeight - contentTop - bottomHints},
        GENRE_COUNT, selectedIndex_, [](int index) -> std::string { return I18N.get(GENRES[index].label); }, nullptr,
        nullptr, nullptr, false, nullptr);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state_ == AUTHOR_LIST) {
    if (authors_.empty()) {
      renderer.drawCenteredTextOffset(UI_10_FONT_ID, centerY, tr(STR_NO_RESULTS), true, centerOffset);
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer, Rect{layout.content.x, contentTop, layout.content.width, pageHeight - contentTop - bottomHints},
          static_cast<int>(authors_.size()), selectedIndex_,
          [this](int index) -> std::string { return authors_[index].name; }, nullptr, nullptr,
          [this](int index) -> std::string {
            char buf[24];
            if (favoritesManager_.isFavorited(authors_[index].id)) {
              snprintf(buf, sizeof(buf), "* %d", authors_[index].workCount);
            } else {
              snprintf(buf, sizeof(buf), "%d", authors_[index].workCount);
            }
            return buf;
          },
          false, nullptr);

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      // X3's horizontal side-button hint overlaps a dense author list. The
      // controls stay active there, but only X4 shows the side labels.
      if (!gpio.deviceIsX3()) {
        GUI.drawSideButtonHints(renderer, tr(STR_PREVIOUS), tr(STR_NEXT));
      }
    }

  } else if (state_ == WORK_LIST) {
    if (works_.empty()) {
      renderer.drawCenteredTextOffset(UI_10_FONT_ID, centerY, tr(STR_NO_RESULTS), true, centerOffset);
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      // ページ情報をヘッダー下に表示
      if (worksTotal_ > WORKS_PAGE_SIZE) {
        char pageInfo[48];
        int currentPage = (worksOffset_ / WORKS_PAGE_SIZE) + 1;
        int totalPages = (worksTotal_ + WORKS_PAGE_SIZE - 1) / WORKS_PAGE_SIZE;
        snprintf(pageInfo, sizeof(pageInfo), "%d/%d (%d)", currentPage, totalPages, worksTotal_);
        renderer.drawText(UI_10_FONT_ID, layout.content.x + layout.content.width - metrics.contentSidePadding - 80,
                          contentTop, pageInfo);
      }

      const int listTop = (worksTotal_ > WORKS_PAGE_SIZE) ? contentTop + lineHeight + 4 : contentTop;
      GUI.drawList(
          renderer, Rect{layout.content.x, listTop, layout.content.width, pageHeight - listTop - bottomHints},
          static_cast<int>(works_.size()), selectedIndex_,
          [this](int index) -> std::string { return works_[index].title; }, nullptr, nullptr,
          [this](int index) -> std::string {
            if (indexManager_.isDownloaded(works_[index].id)) {
              return tr(STR_DOWNLOADED_BOOKS);
            }
            return "";
          },
          false, nullptr);

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }

  } else if (state_ == WORK_DETAIL) {
    int y = contentTop;

    renderer.drawText(UI_12_FONT_ID, contentLeft, y, selectedWorkTitle_);
    y += renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;

    if (selectedWorkAuthor_[0] != '\0') {
      renderer.drawText(UI_10_FONT_ID, contentLeft, y, selectedWorkAuthor_);
      y += lineHeight + metrics.verticalSpacing;
    }

    bool alreadyDownloaded = indexManager_.isDownloaded(selectedWorkId_);
    if (alreadyDownloaded) {
      y += metrics.verticalSpacing;
      renderer.drawText(UI_10_FONT_ID, contentLeft, y, tr(STR_DOWNLOAD_COMPLETE));

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", tr(STR_DELETE_CONFIRM), tr(STR_AOZORA_UPDATE));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", tr(STR_AOZORA_GET));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }

  } else if (state_ == DOWNLOADING) {
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, centerY - lineHeight, tr(STR_DOWNLOADING_BOOK), true, centerOffset);

    float progress = 0;
    if (downloadTotal_ > 0) {
      progress = static_cast<float>(downloadProgress_) / static_cast<float>(downloadTotal_);
    }

    int barY = centerY + metrics.verticalSpacing;
    GUI.drawProgressBar(renderer, Rect{contentLeft, barY, contentWidth, metrics.progressBarHeight}, downloadProgress_,
                        downloadTotal_);

    int percentY = barY + metrics.progressBarHeight + metrics.verticalSpacing;
    char buf[32];
    if (downloadTotal_ > 0) {
      snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(progress * 100));
    } else if (downloadProgress_ > 0) {
      snprintf(buf, sizeof(buf), "%d KB", static_cast<int>(downloadProgress_ / 1024));
    } else {
      snprintf(buf, sizeof(buf), "...");
    }
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, percentY, buf, true, centerOffset);

  } else if (state_ == FAVORITE_AUTHORS) {
    const auto& favEntries = favoritesManager_.entries();

    if (favEntries.empty()) {
      renderer.drawCenteredTextOffset(UI_10_FONT_ID, centerY, tr(STR_NO_FAVORITE_AUTHORS), true, centerOffset);
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      GUI.drawList(
          renderer, Rect{layout.content.x, contentTop, layout.content.width, pageHeight - contentTop - bottomHints},
          static_cast<int>(favEntries.size()), selectedIndex_,
          [&favEntries](int index) -> std::string { return favEntries[index].name; }, nullptr, nullptr, nullptr, false,
          nullptr);

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }

  } else if (state_ == AUTHOR_ACTION) {
    renderer.drawText(UI_12_FONT_ID, contentLeft, contentTop, selectedAuthorName_);
    const int listTop = contentTop + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;

    bool isFav = favoritesManager_.isFavorited(selectedAuthorId_);
    GUI.drawList(
        renderer, Rect{layout.content.x, listTop, layout.content.width, pageHeight - listTop - bottomHints}, 2,
        actionMenuIndex_,
        [isFav](int index) -> std::string {
          if (index == 0) return tr(STR_VIEW_WORKS);
          return isFav ? tr(STR_REMOVE_FROM_FAVORITES) : tr(STR_ADD_TO_FAVORITES);
        },
        nullptr, nullptr, nullptr, false, nullptr);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state_ == DOWNLOADED_LIST) {
    const int total = static_cast<int>(indexManager_.activeCount());

    if (total == 0) {
      renderer.drawCenteredTextOffset(UI_10_FONT_ID, centerY, tr(STR_NO_RESULTS), true, centerOffset);
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      // 選択中のエントリを含むページをキャッシュに載せる
      const int pageStart = (selectedIndex_ / DL_PAGE_SIZE) * DL_PAGE_SIZE;
      if (dlPageStart_ != pageStart) {
        loadDownloadedPage(pageStart);
      }

      GUI.drawList(
          renderer, Rect{layout.content.x, contentTop, layout.content.width, pageHeight - contentTop - bottomHints},
          total, selectedIndex_,
          [this](int index) -> std::string {
            const int localIdx = index - dlPageStart_;
            if (localIdx < 0 || localIdx >= dlPageCount_) return "";
            return dlPageCache_[localIdx].title;
          },
          nullptr, nullptr,
          [this](int index) -> std::string {
            const int localIdx = index - dlPageStart_;
            if (localIdx < 0 || localIdx >= dlPageCount_) return "";
            return dlPageCache_[localIdx].author;
          },
          false, nullptr);

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }

  } else if (state_ == ERROR) {
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, centerY - lineHeight, tr(STR_ERROR_MSG), true, centerOffset,
                                    EpdFontFamily::BOLD);
    if (!errorMessage_.empty()) {
      renderer.drawCenteredTextOffset(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str(), true,
                                      centerOffset);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

void AozoraActivity::loadDownloadedPage(int start) {
  const int total = static_cast<int>(indexManager_.activeCount());
  if (start < 0) start = 0;

  dlPageStart_ = start;
  dlPageCount_ = 0;

  for (int i = 0; i < DL_PAGE_SIZE; ++i) {
    const int absIndex = start + i;
    if (absIndex >= total) break;
    if (!indexManager_.readEntryAt(absIndex, dlPageCache_[i])) {
      LOG_ERR("AOZORA", "loadDownloadedPage: readEntryAt(%d) failed", absIndex);
      break;
    }
    dlPageCount_++;
  }
}
