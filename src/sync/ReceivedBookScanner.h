#pragma once
#include <ArduinoJson.h>
#include <HalStorage.h>

#include <array>
#include <functional>
#include <string>
#include <vector>
namespace yomuka::sync {
enum class ReceivedStatus : uint8_t { Pending, Matched, Missing, Error };
enum class ReceivedError : uint8_t { None, File, Snapshot, Source, Range, Search, Memory };
struct ReceivedBook {
  uint64_t id = 0;
  std::string file, path, title;
  ReceivedStatus status = ReceivedStatus::Pending;
  ReceivedError error = ReceivedError::None;
  uint8_t units = 0;
  uint32_t spines = 0, spineIndex = 0, chapterPage = 0, bookmarks = 0;
  std::array<uint32_t, 4> dates{};
};
// Discovery and target validation only. Never recovers files, loads Epub, or imports reader data.
class ReceivedBookScanner {
  enum class Phase { Inbox, Search, Verify, Done };
  Phase phase = Phase::Inbox;
  struct Directory {
    FsFile file;
    std::string path;
  };
  FsFile inbox;
  std::vector<Directory> directories;
  std::vector<ReceivedBook> entries;
  std::vector<std::string> requested;
  size_t requestedIndex = 0, verifyIndex = 0;
  bool started = false;
  bool load(ReceivedBook& entry, JsonDocument& doc);
  void inspect(const std::string& path);
  void search();

 public:
  explicit ReceivedBookScanner(std::vector<std::string> files = {}) : requested(std::move(files)) {}
  size_t warnings = 0, checkedFiles = 0;
  bool step(const std::function<bool()>& cancel = {});
  bool done() const { return phase == Phase::Done; }
  bool searchingSd() const { return phase == Phase::Search; }
  const std::vector<ReceivedBook>& books() const { return entries; }
  size_t count(ReceivedStatus status) const;
};
}  // namespace yomuka::sync
