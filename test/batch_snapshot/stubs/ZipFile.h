#pragma once
#include <HalStorage.h>
#include <Print.h>

#include <cstdlib>
class ZipFile {
  std::string path;

 public:
  explicit ZipFile(const std::string& p) : path(p) {}
  bool getArchiveFingerprint(uint64_t* id) {
    auto found = Storage.files.find(path);
    if (!Storage.online || found == Storage.files.end()) return false;
    *id = std::strtoull(found->second->c_str(), nullptr, 16);
    return true;
  }
  bool getInflatedFileSize(const char* name, size_t* size) {
    auto found = Storage.files.find(path + "::" + name);
    if (found == Storage.files.end()) return false;
    *size = found->second->size();
    return true;
  }
  bool readFileToStream(const char* name, Print& output, size_t chunk) {
    auto found = Storage.files.find(path + "::" + name);
    if (found == Storage.files.end()) return false;
    const auto& text = *found->second;
    for (size_t at = 0; at < text.size();) {
      const auto length = std::min(chunk, text.size() - at);
      if (output.write(reinterpret_cast<const uint8_t*>(text.data() + at), length) != length) return false;
      at += length;
    }
    return true;
  }
};
