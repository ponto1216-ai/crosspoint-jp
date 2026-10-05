#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

struct WifiResult {
  bool connected = false;
  std::string ssid;
  std::string ip;
};

struct KeyboardResult {
  std::string text;
};

struct MenuResult {
  int action = -1;
  uint8_t orientation = 0;
  uint8_t pageTurnOption = 0;
  bool layoutChanged = false;
};

struct ChapterResult {
  int spineIndex = 0;
};

struct PercentResult {
  int percent = 0;
};

struct PageResult {
  uint32_t page = 0;
};

struct BookmarkResult {
  uint16_t spineIndex = 0;
  uint16_t chapterPage = 0;
  uint16_t chapterPageCount = 0;
  float percentage = 0.0f;
};

struct SyncResult {
  int spineIndex = 0;
  int page = 0;
};

enum class NetworkMode;

struct NetworkModeResult {
  NetworkMode mode;
};

struct FootnoteResult {
  std::string href;
};

struct FilePathResult {
  std::string path;
};

struct ReceivedFilesResult {
  std::vector<std::string> files;
};

using ResultVariant =
    std::variant<std::monostate, WifiResult, KeyboardResult, MenuResult, ChapterResult, PercentResult, PageResult,
                 BookmarkResult, SyncResult, NetworkModeResult, FootnoteResult, FilePathResult, ReceivedFilesResult>;

struct ActivityResult {
  bool isCancelled = false;
  ResultVariant data;

  explicit ActivityResult() = default;

  template <typename ResultType>
    requires std::is_constructible_v<ResultVariant, ResultType&&>
  // cppcheck-suppress noExplicitConstructor
  ActivityResult(ResultType&& result) : data{std::forward<ResultType>(result)} {}
};

using ActivityResultHandler = std::function<void(const ActivityResult&)>;
