#pragma once
#include <vector>

#include "StorageIo.h"
#include "SyncProgress.h"

namespace yomuka {
namespace sync {
ReadStatus readProgress(const std::string& path, Progress& value, uint32_t& updatedAt);
// Pure validator shared by normal reads and discovery without file recovery.
bool decodeSavedProgressTime(const uint8_t* progress, size_t length, const uint8_t* metadata, size_t size,
                             uint32_t& timestamp);
// Timestamp preparation contains both exact old/new payloads. Reading exposes
// only the timestamp matching the committed progress bytes. No import API yet.
bool saveProgress(const std::string& path, const uint8_t* bytes, size_t length, bool recordLocalChange = true);
bool prepareProgressTime(const std::string& path, const uint8_t* bytes, size_t length, uint32_t updatedAt,
                         std::vector<uint8_t>& output);
}  // namespace sync
}  // namespace yomuka
