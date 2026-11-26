#include "TxtProgressMigration.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>

#include <algorithm>
#include <array>

#include "activities/reader/ProgressFile.h"

namespace {
constexpr uint32_t INDEX_MAGIC = 0x54585449;
constexpr uint8_t INDEX_VERSION = 6;
constexpr size_t INDEX_HEADER_SIZE = 30;
// An EPUB progress record can hold a 16-bit page/count pair. Use it as a
// relative position until the first layout provides the actual page count.
constexpr uint16_t RESUME_SCALE = UINT16_MAX;

uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

enum class IndexResult { Valid, Unavailable, Failed };

IndexResult readIndexPosition(const Txt& text, const uint32_t page, double& fraction) {
  const std::string path = text.getCachePath() + "/index.bin";
  if (!Storage.exists(path.c_str())) return IndexResult::Unavailable;
  FsFile file;
  if (!Storage.openFileForRead("TXM", path, file)) return IndexResult::Failed;
  std::array<uint8_t, INDEX_HEADER_SIZE> header{};
  if (file.read(header.data(), header.size()) != static_cast<int>(header.size())) {
    return file.getError() ? IndexResult::Failed : IndexResult::Unavailable;
  }
  const uint32_t pages = readLe32(header.data() + 26);
  const uint32_t width = readLe32(header.data() + 9);
  const uint32_t lines = readLe32(header.data() + 13);
  const uint32_t margin = readLe32(header.data() + 21);
  const uint64_t expectedSize = INDEX_HEADER_SIZE + static_cast<uint64_t>(pages) * sizeof(uint32_t);
  if (readLe32(header.data()) != INDEX_MAGIC || header[4] != INDEX_VERSION ||
      readLe32(header.data() + 5) != text.getFileSize() || pages == 0 || page >= pages ||
      static_cast<uint64_t>(pages) > static_cast<uint64_t>(text.getFileSize()) + 1 ||
      expectedSize != file.size() || width == 0 || width > INT32_MAX || lines == 0 ||
      lines > INT32_MAX || margin > INT32_MAX) return IndexResult::Unavailable;

  // Validate the entire table with bounded memory before trusting any offset.
  uint8_t offsets[256];
  uint32_t previous = 0;
  uint32_t selected = 0;
  for (uint32_t first = 0; first < pages;) {
    const uint32_t count = std::min<uint32_t>(pages - first, sizeof(offsets) / sizeof(uint32_t));
    const size_t bytes = count * sizeof(uint32_t);
    if (file.read(offsets, bytes) != static_cast<int>(bytes)) return IndexResult::Failed;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t offset = readLe32(offsets + i * sizeof(uint32_t));
      if (offset > text.getFileSize() ||
          (first + i == 0 ? offset != text.getContentOffset() : offset <= previous)) {
        return IndexResult::Unavailable;
      }
      if (first + i == page) selected = offset;
      previous = offset;
    }
    first += count;
    delay(1);
  }
  const size_t contentSize = text.getFileSize() - text.getContentOffset();
  fraction = contentSize > 0 ? static_cast<double>(selected - text.getContentOffset()) / contentSize : 0.0;
  return IndexResult::Valid;
}
}  // namespace

TxtProgressMigration::Result TxtProgressMigration::migrate(const std::string& filepath,
                                                         const std::string& cacheRoot,
                                                         const std::string& readerCachePath,
                                                         const uint16_t spineCount) {
  const HalStorage::StorageLock lock;
  const std::string destination = readerCachePath + "/progress.bin";
  if (!yomuka::sync::recoverFile(destination)) return Result::Failed;
  if (Storage.exists(destination.c_str())) return Result::Unchanged;

  Txt text(filepath, cacheRoot);
  const std::string legacyPath = text.getCachePath() + "/progress.bin";
  if (!yomuka::sync::recoverFile(legacyPath)) return Result::Failed;
  if (!Storage.exists(legacyPath.c_str())) return Result::Unchanged;
  FsFile legacy;
  if (!Storage.openFileForRead("TXM", legacyPath, legacy)) return Result::Failed;
  const size_t length = legacy.size();
  if (length < 4 || length > 6) return Result::NoPosition;
  uint8_t old[6] = {};
  if (legacy.read(old, length) != static_cast<int>(length)) return Result::Failed;
  legacy.close();
  if ((length >= 5 && old[4] > 1) ||
      (length == 6 && old[5] > 100 && old[5] != UINT8_MAX)) return Result::NoPosition;
  if (spineCount == 0) return Result::Failed;

  const bool finished = length >= 5 && old[4] == 1;
  double fraction = 0.0;
  if (!finished) {
    if (!text.load()) return Result::Failed;
    const uint32_t page = readLe32(old);
    const IndexResult index = readIndexPosition(text, page, fraction);
    if (index == IndexResult::Failed) return Result::Failed;
    if (index == IndexResult::Unavailable) {
      if (length == 6 && old[5] <= 100) fraction = old[5] / 100.0;
      else if (page != 0) return Result::NoPosition;
    }
  }

  const uint16_t spine = finished ? spineCount : 0;
  const uint16_t page = finished ? 0 : static_cast<uint16_t>(
      std::min<uint32_t>(RESUME_SCALE - 1, static_cast<uint32_t>(fraction * RESUME_SCALE)));
  const uint16_t count = finished ? 0 : RESUME_SCALE;
  const uint8_t percent = finished ? 100 : static_cast<uint8_t>(fraction * 100);
  const uint8_t record[8] = {
      static_cast<uint8_t>(spine), static_cast<uint8_t>(spine >> 8),
      static_cast<uint8_t>(page), static_cast<uint8_t>(page >> 8),
      static_cast<uint8_t>(count), static_cast<uint8_t>(count >> 8),
      static_cast<uint8_t>(finished), percent};
  if (!ProgressFile::writeAtomicPath(destination, record, sizeof(record), false)) return Result::Failed;
  LOG_INF("TXM", "Migrated legacy TXT progress: finished=%d position=%u%%", finished, percent);
  return Result::Migrated;
}
