#pragma once
#include <HalStorage.h>

namespace yomuka::sync {
inline std::string failedRecovery;
inline bool interruptedWrite = false;
inline bool recoverFile(const std::string& path) { return path != failedRecovery; }
// Model the existing StorageIo contract at the migration's persistence boundary.
// A failed publication can leave a temporary file, while the destination remains absent.
inline bool writeBytes(const std::string& path, const uint8_t* data, size_t length) {
  if (interruptedWrite) {
    Storage.files[path + ".tmp"] = std::string(reinterpret_cast<const char*>(data), length / 2);
    return false;
  }
  Storage.files[path] = std::string(reinterpret_cast<const char*>(data), length);
  Storage.files.erase(path + ".tmp");
  return true;
}
inline bool saveProgress(const std::string& path, const uint8_t* data, size_t length, bool) {
  return writeBytes(path, data, length);
}
}  // namespace yomuka::sync
