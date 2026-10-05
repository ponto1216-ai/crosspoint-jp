#pragma once
#include <HalStorage.h>

// The scan tests mock only the existing ZIP fingerprint boundary. Content below
// is a 16-digit test identity, not a real EPUB. Device builds use actual ZipFile.
class ZipFile {
  const std::string& path;

 public:
  explicit ZipFile(const std::string& path) : path(path) {}
  bool getArchiveFingerprint(uint64_t* id) {
    const auto it = Storage.files.find(path);
    return it != Storage.files.end() && yomuka::sync::ShareCandidateCatalog::parseBookId(it->second->c_str(), *id);
  }
};
