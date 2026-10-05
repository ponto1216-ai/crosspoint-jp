#include "ProgressStorage.h"

#include <HalStorage.h>

#include <cstring>

namespace yomuka {
namespace sync {
namespace {
constexpr size_t kMetadataSize = 30;
constexpr size_t kSlotSize = 13;
void putTime(uint8_t* bytes, uint32_t timestamp) {
  for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<uint8_t>(timestamp >> (8U * i));
}
uint32_t getTime(const uint8_t* bytes) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(bytes[i]) << (8U * i);
  return value;
}
ReadStatus matchingTime(const std::string& path, const uint8_t* progress, size_t length, uint32_t& timestamp) {
  uint8_t metadata[kMetadataSize] = {};
  size_t size = 0;
  const auto status = readBytes(path + ".sync-time", metadata, sizeof(metadata), size);
  if (status == ReadStatus::Absent) {
    timestamp = 0;
    return ReadStatus::Present;
  }
  if (status != ReadStatus::Present) return status == ReadStatus::LimitExceeded ? ReadStatus::Corrupt : status;
  return decodeSavedProgressTime(progress, length, metadata, size, timestamp) ? ReadStatus::Present
                                                                              : ReadStatus::Corrupt;
}
}  // namespace

bool decodeSavedProgressTime(const uint8_t* progress, size_t length, const uint8_t* metadata, size_t size,
                             uint32_t& timestamp) {
  if (!progress || length > 8 || !metadata || size != kMetadataSize || std::memcmp(metadata, "YSM1", 4) != 0)
    return false;
  for (size_t i = 0; i < 2; ++i) {
    const uint8_t* slot = metadata + 4 + kSlotSize * i;
    if (slot[4] > 8) return false;
  }
  // The candidate slot comes first. A local no-op never prepares new metadata.
  for (size_t i = 0; i < 2; ++i) {
    const uint8_t* slot = metadata + 4 + kSlotSize * i;
    if (slot[4] == length && std::memcmp(slot + 5, progress, length) == 0) {
      timestamp = getTime(slot);
      return true;
    }
  }
  // Legacy/external writes did not provide a date for these bytes.
  timestamp = 0;
  return true;
}

ReadStatus readProgress(const std::string& path, Progress& value, uint32_t& updatedAt) {
  HalStorage::StorageLock lock;
  uint8_t bytes[8] = {};
  size_t length = 0;
  const auto status = readBytes(path, bytes, sizeof(bytes), length);
  if (status != ReadStatus::Present) return status == ReadStatus::LimitExceeded ? ReadStatus::Corrupt : status;
  Progress candidate;
  if (decodeLegacyProgress(bytes, length, candidate) != ProgressError::None) return ReadStatus::Corrupt;
  uint32_t timestamp = 0;
  const auto metadataStatus = matchingTime(path, bytes, length, timestamp);
  if (metadataStatus != ReadStatus::Present) return metadataStatus;
  value = candidate;
  updatedAt = timestamp;
  return ReadStatus::Present;
}

bool saveProgress(const std::string& path, const uint8_t* bytes, size_t length, bool recordLocalChange) {
  HalStorage::StorageLock lock;
  if (!bytes || (length != 4 && length != 6 && length != 7 && length != 8)) return false;
  uint8_t previous[8] = {};
  size_t previousLength = 0;
  const auto status = readBytes(path, previous, sizeof(previous), previousLength);
  if (status != ReadStatus::Present && status != ReadStatus::Absent) return false;
  if (status == ReadStatus::Present && previousLength == length && std::memcmp(previous, bytes, length) == 0)
    return true;
  uint32_t previousTime = 0;
  if (status == ReadStatus::Present &&
      matchingTime(path, previous, previousLength, previousTime) != ReadStatus::Present)
    return false;
  uint8_t metadata[kMetadataSize] = {};
  std::memcpy(metadata, "YSM1", 4);
  putTime(metadata + 4, recordLocalChange ? localUpdateTime() : 0);
  metadata[8] = static_cast<uint8_t>(length);
  std::memcpy(metadata + 9, bytes, length);
  putTime(metadata + 4 + kSlotSize, previousTime);
  metadata[8 + kSlotSize] = static_cast<uint8_t>(previousLength);
  std::memcpy(metadata + 9 + kSlotSize, previous, previousLength);
  // If preparation fails, progress is untouched. If publishing progress fails,
  // the old bytes still select the old timestamp, including after .bak recovery.
  return writeBytes(path + ".sync-time", metadata, sizeof(metadata)) && writeBytes(path, bytes, length);
}
bool prepareProgressTime(const std::string& path, const uint8_t* bytes, size_t length, uint32_t updatedAt,
                         std::vector<uint8_t>& output) {
  HalStorage::StorageLock lock;
  Progress checked;
  if (decodeLegacyProgress(bytes, length, checked) != ProgressError::None) return false;
  uint8_t previous[8] = {};
  size_t oldLength = 0;
  const auto status = readBytes(path, previous, sizeof(previous), oldLength);
  if (status != ReadStatus::Present && status != ReadStatus::Absent) return false;
  uint32_t oldTime = 0;
  if (status == ReadStatus::Present && matchingTime(path, previous, oldLength, oldTime) != ReadStatus::Present)
    return false;
  std::vector<uint8_t> metadata(kMetadataSize, 0);
  std::memcpy(metadata.data(), "YSM1", 4);
  putTime(metadata.data() + 4, updatedAt);
  metadata[8] = length;
  std::memcpy(metadata.data() + 9, bytes, length);
  putTime(metadata.data() + 4 + kSlotSize, oldTime);
  metadata[8 + kSlotSize] = oldLength;
  std::memcpy(metadata.data() + 9 + kSlotSize, previous, oldLength);
  output = std::move(metadata);
  return true;
}
}  // namespace sync
}  // namespace yomuka
