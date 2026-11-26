#pragma once
#include <Print.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
inline size_t failReadAt = std::numeric_limits<size_t>::max();
inline std::string failWritePath;
inline std::string failRenamePath;
inline std::string failRemovePath;
class FsFile : public Print {
 public:
  std::string* bytes = nullptr;
  std::string path;
  size_t offset = 0;
  bool writeError = false;
  explicit operator bool() const { return bytes != nullptr; }
  size_t size() const { return bytes->size(); }
  size_t position() const { return offset; }
  bool seek(size_t position) { offset = position; return offset <= size(); }
  int available() const { return bytes && offset < size(); }
  int read() {
    if (offset >= failReadAt) return -1;
    return available() ? static_cast<unsigned char>((*bytes)[offset++]) : -1;
  }
  int read(void* target, size_t count) {
    if (offset >= failReadAt) return -1;
    count = std::min(count, size() - offset);
    std::memcpy(target, bytes->data() + offset, count);
    offset += count;
    return static_cast<int>(count);
  }
  size_t write(const uint8_t* data, size_t count) override {
    if (path == failWritePath) { writeError = true; return 0; }
    if (offset + count > bytes->size()) bytes->resize(offset + count);
    std::memcpy(bytes->data() + offset, data, count);
    offset += count;
    return count;
  }
  size_t write(const void* data, size_t count) { return write(static_cast<const uint8_t*>(data), count); }
  uint8_t getError() const { return writeError ? 1 : 0; }
  void flush() {}
  bool close() { bytes = nullptr; return true; }
};
struct HalStorage {
  struct StorageLock { ~StorageLock() {} };
  std::map<std::string, std::string> files;
  std::set<std::string> directories;
  size_t writes = 0;
  bool exists(const char* path) { return files.count(path) || directories.count(path); }
  bool mkdir(const char* path) { directories.insert(path); return true; }
  bool remove(const char* path) { return path != failRemovePath && files.erase(path); }
  bool removeDir(const char* path) {
    if (path == failRemovePath) return false;
    const std::string prefix = std::string(path) + "/";
    for (auto it = files.begin(); it != files.end();) {
      if (it->first.compare(0, prefix.size(), prefix) == 0) it = files.erase(it);
      else ++it;
    }
    directories.erase(path);
    return true;
  }
  bool rename(const char* from, const char* to) {
    if (to == failRenamePath || !files.count(from) || files.count(to)) return false;
    files[to] = files.at(from);
    files.erase(from);
    return true;
  }
  bool openFileForRead(const char*, const std::string& path, FsFile& file) {
    auto found = files.find(path);
    if (found == files.end()) return false;
    file.bytes = &found->second;
    file.path = path;
    file.offset = 0;
    file.writeError = false;
    return true;
  }
  bool openFileForWrite(const char*, const std::string& path, FsFile& file) {
    file.bytes = &files[path];
    file.bytes->clear();
    file.path = path;
    file.offset = 0;
    file.writeError = false;
    ++writes;
    return true;
  }
  bool readFileToStream(const char* path, Print& output, size_t chunkSize) {
    if (chunkSize == 0) return false;
    FsFile file;
    if (!openFileForRead("TEST", path, file)) return false;
    uint8_t buffer[1024];
    int count = 0;
    while ((count = file.read(buffer, std::min(chunkSize, sizeof(buffer)))) > 0) {
      if (output.write(buffer, count) != static_cast<size_t>(count)) return false;
    }
    return count == 0;
  }
};
inline HalStorage Storage;
