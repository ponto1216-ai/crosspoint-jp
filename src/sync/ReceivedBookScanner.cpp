#include "ReceivedBookScanner.h"

#include <ZipFile.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>

#include "SnapshotBookProbe.h"
#include "SnapshotUpload.h"
#include "SnapshotValidation.h"
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif
namespace yomuka::sync {
namespace {
bool extension(const std::string& path, const char* ext) {
  const size_t n = std::char_traits<char>::length(ext);
  if (path.size() < n) return false;
  for (size_t i = 0; i < n; ++i)
    if (std::tolower(static_cast<unsigned char>(path[path.size() - n + i])) != ext[i]) return false;
  return true;
}
bool nameOf(FsFile& file, std::string& name) {
  std::array<char, 512> buffer{};
  auto size = file.getName(buffer.data(), buffer.size());
  if (!size || size >= buffer.size() - 1) return false;
  name = buffer.data();
  auto slash = name.find_last_of('/');
  if (slash != std::string::npos) name.erase(0, slash + 1);
  return !name.empty() && name != "." && name != "..";
}
bool admitted(size_t size) {
#if defined(ARDUINO_ARCH_ESP32)
  return SnapshotUpload::memoryAvailable(size, heap_caps_get_free_size(MALLOC_CAP_8BIT),
                                         heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#else
  return size > 0 && size <= 65536;
#endif
}
}  // namespace
bool ReceivedBookScanner::load(ReceivedBook& entry, JsonDocument& doc) {
  auto file = Storage.open(entry.file.c_str());
  entry.status = ReceivedStatus::Error;
  entry.error = ReceivedError::File;
  if (!file || file.isDirectory()) return false;
  const auto size = file.size();
  if (!size || size > 65536) {
    entry.error = ReceivedError::Snapshot;
    return false;
  }
  if (!admitted(size)) {
    entry.error = ReceivedError::Memory;
    return false;
  }
  std::unique_ptr<uint8_t[]> bytes(new (std::nothrow) uint8_t[size]);
  if (!bytes) {
    entry.error = ReceivedError::Memory;
    return false;
  }
  size_t read = 0;
  while (read < size) {
    const int got = file.read(bytes.get() + read, size - read);
    if (got <= 0) return false;
    read += got;
  }
  const bool stable = file.size() == size;
  if (!file.close() || !stable) return false;
  if (parseSnapshot(bytes.get(), size, doc) != SnapshotError::None) {
    entry.error = ReceivedError::Snapshot;
    return false;
  }
  const std::string id = doc["bookId"].as<const char*>();
  // A published inbox file is canonical. Ignore temporary/backup files, reject identity/filename mismatches.
  if (entry.file != "/YomukaSync/" + id + ".json") {
    entry.error = ReceivedError::Snapshot;
    return false;
  }
  uint64_t value = 0;
  for (char c : id) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      entry.error = ReceivedError::Snapshot;
      return false;
    }
    value = (value << 4) | (c <= '9' ? c - '0' : c - 'a' + 10);
  }
  if (id.size() != 16 || !value) {
    entry.error = ReceivedError::Snapshot;
    return false;
  }
  entry.id = value;
  entry.units = 0;
  constexpr const char* names[] = {"progress", "bookmarks", "readerSettings", "history"};
  for (unsigned i = 0; i < 4; ++i)
    if (!doc["units"][names[i]].isUnbound()) {
      entry.units |= 1 << i;
      entry.dates[i] = doc["units"][names[i]]["updatedAt"] | 0u;
    }
  entry.spineIndex = doc["units"]["progress"]["data"]["spineIndex"] | 0u;
  entry.chapterPage = doc["units"]["progress"]["data"]["chapterPage"] | 0u;
  entry.bookmarks = doc["units"]["bookmarks"]["data"].size();
  entry.status = ReceivedStatus::Pending;
  entry.error = ReceivedError::None;
  return true;
}
void ReceivedBookScanner::inspect(const std::string& path) {
  if (std::any_of(entries.begin(), entries.end(), [&](const ReceivedBook& e) { return e.file == path; })) return;
  if (entries.size() >= 512) {
    ++warnings;
    return;
  }
#if defined(ARDUINO_ARCH_ESP32)
  const size_t need = (entries.size() + 1) * sizeof(ReceivedBook) * 2 + 8192;
  if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < need + 16384 ||
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < need) {
    ++warnings;
    return;
  }
#endif
  ReceivedBook entry;
  entry.file = path;
  entry.title = path.substr(path.find_last_of('/') + 1);
  JsonDocument doc;
  load(entry, doc);
  entries.push_back(std::move(entry));
}
void ReceivedBookScanner::search() {
  if (directories.empty()) {
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.file < b.file; });
    phase = Phase::Verify;
    return;
  }
  auto file = directories.back().file.openNextFile();
  if (!file) {
    if (directories.back().file.getError()) ++warnings;
    directories.pop_back();
    return;
  }
  std::string name;
  if (!nameOf(file, name)) {
    ++warnings;
    return;
  }
  if (name.front() == '.') return;
  const auto path = directories.back().path + "/" + name;
  if (path == "/YomukaSync") return;
  if (file.isDirectory()) {
    if (directories.size() >= 32)
      ++warnings;
    else
      directories.push_back({std::move(file), path});
    return;
  }
  if (!extension(path, ".epub")) return;
  file.close();
  ++checkedFiles;
  uint64_t id = 0;
  ZipFile zip(path);
  if (!zip.getArchiveFingerprint(&id) || !id) {
    ++warnings;
    return;
  }
  for (auto& entry : entries)
    if (entry.status == ReceivedStatus::Pending && entry.id == id && (entry.path.empty() || path < entry.path)) {
      entry.path = path;
      entry.title = name.substr(0, name.size() - 5);
    }
}
bool ReceivedBookScanner::step(const std::function<bool()>& cancel) {
  if (done()) return true;
  if (cancel && cancel()) {
    ++warnings;
    directories.clear();
    if (inbox) inbox.close();
    for (auto& entry : entries)
      if (entry.status == ReceivedStatus::Pending) {
        entry.status = ReceivedStatus::Error;
        entry.error = ReceivedError::Search;
      }
    phase = Phase::Done;
    return true;
  }
  if (!Storage.ready()) {
    ++warnings;
    directories.clear();
    if (inbox) inbox.close();
    for (auto& e : entries)
      if (e.status == ReceivedStatus::Pending) {
        e.status = ReceivedStatus::Error;
        e.error = ReceivedError::Search;
      }
    phase = Phase::Done;
    return true;
  }
  if (phase == Phase::Inbox) {
    if (!started) {
      started = true;
      if (requested.empty() && Storage.exists("/YomukaSync")) {
        inbox = Storage.open("/YomukaSync");
        if (!inbox || !inbox.isDirectory()) ++warnings;
      }
    }
    if (!requested.empty() && requestedIndex < requested.size()) {
      inspect(requested[requestedIndex++]);
      return false;
    }
    auto file = requested.empty() && inbox ? inbox.openNextFile() : FsFile{};
    if (file) {
      std::string name;
      if (!file.isDirectory()) {
        if (!nameOf(file, name))
          ++warnings;
        else if (extension(name, ".json")) {
          file.close();
          inspect("/YomukaSync/" + name);
        }
      }
      return false;
    }
    if (inbox && inbox.getError()) ++warnings;
    if (inbox) inbox.close();
    if (std::any_of(entries.begin(), entries.end(),
                    [](const auto& e) { return e.status == ReceivedStatus::Pending; })) {
      auto root = Storage.open("/");
      if (root && root.isDirectory())
        directories.push_back({std::move(root), ""});
      else
        ++warnings;
    }
    phase = Phase::Search;
    return false;
  }
  if (phase == Phase::Search) {
    search();
    return false;
  }
  if (verifyIndex == entries.size()) {
    phase = Phase::Done;
    return true;
  }
  auto& entry = entries[verifyIndex++];
  if (entry.status != ReceivedStatus::Pending) return false;
  if (entry.path.empty()) {
    entry.status = warnings ? ReceivedStatus::Error : ReceivedStatus::Missing;
    entry.error = warnings ? ReceivedError::Search : ReceivedError::None;
    return false;
  }
  if (!probeSnapshotBook(entry.path, entry.id, entry.spines, cancel)) {
    entry.status = ReceivedStatus::Error;
    entry.error = ReceivedError::Source;
    return false;
  }
  JsonDocument doc;
  const auto original = entry.id;
  if (!load(entry, doc)) return false;
  if (entry.id != original ||
      validateSnapshotTarget(doc, doc["bookId"].as<const char*>(), entry.spines) != SnapshotError::None) {
    entry.status = ReceivedStatus::Error;
    entry.error = ReceivedError::Range;
    return false;
  }
  entry.status = ReceivedStatus::Matched;
  return false;
}
size_t ReceivedBookScanner::count(ReceivedStatus status) const {
  return std::count_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.status == status; });
}
}  // namespace yomuka::sync
