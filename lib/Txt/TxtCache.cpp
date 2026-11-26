#include "Txt.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <algorithm>

namespace {
constexpr uint32_t CACHE_MAGIC = 0x48545854;  // "TXTH"
constexpr uint8_t CACHE_VERSION = 1;
constexpr size_t MARKER_SIZE = sizeof(CACHE_MAGIC) + sizeof(CACHE_VERSION) + sizeof(uint64_t) + sizeof(uint32_t);

bool fingerprintSource(const std::string& path, const size_t expectedSize, uint64_t& fingerprint) {
  FsFile source;
  if (!Storage.openFileForRead("TXT", path, source) || source.size() != expectedSize) return false;
  fingerprint = 14695981039346656037ULL;
  uint8_t buffer[1024];
  size_t total = 0;
  while (total < expectedSize) {
    const int count = source.read(buffer, std::min(sizeof(buffer), expectedSize - total));
    if (count <= 0) return false;
    for (int i = 0; i < count; ++i) {
      fingerprint ^= buffer[i];
      fingerprint *= 1099511628211ULL;
    }
    total += count;
    if (total % (64 * 1024) == 0) delay(1);
  }
  return source.size() == expectedSize;
}

bool cacheMatches(const std::string& path, const uint64_t sourceFingerprint) {
  FsFile marker;
  if (!Storage.openFileForRead("TXT", path + "/text.source", marker) || marker.size() != MARKER_SIZE) return false;
  uint32_t magic = 0;
  uint8_t version = 0;
  uint64_t fingerprint = 0;
  uint32_t htmlSize = 0;
  if (marker.read(&magic, sizeof(magic)) != sizeof(magic) ||
      marker.read(&version, sizeof(version)) != sizeof(version) ||
      marker.read(&fingerprint, sizeof(fingerprint)) != sizeof(fingerprint) ||
      marker.read(&htmlSize, sizeof(htmlSize)) != sizeof(htmlSize) || magic != CACHE_MAGIC ||
      version != CACHE_VERSION || fingerprint != sourceFingerprint || htmlSize == 0) return false;
  FsFile html;
  return Storage.openFileForRead("TXT", path + "/text.xhtml", html) && html.size() == htmlSize;
}

bool removeIfPresent(const std::string& path) {
  return !Storage.exists(path.c_str()) || Storage.remove(path.c_str());
}
}  // namespace

bool Txt::isTextFile(const std::string_view path) {
  return FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path);
}

bool Txt::prepareHtmlCache(const std::string& readerCachePath, const bool buildIfMissing) {
  const HalStorage::StorageLock storageLock;
  loaded = false;
  if (!load()) return false;
  uint64_t fingerprint = 0;
  if (!fingerprintSource(filepath, fileSize, fingerprint)) return false;
  if (cacheMatches(readerCachePath, fingerprint)) return true;
  if (!buildIfMissing) return false;
  if (!Storage.exists(readerCachePath.c_str()) && !Storage.mkdir(readerCachePath.c_str())) return false;

  const std::string htmlPath = readerCachePath + "/text.xhtml";
  const std::string htmlTmp = htmlPath + ".tmp";
  const std::string markerPath = readerCachePath + "/text.source";
  const std::string markerTmp = markerPath + ".tmp";
  if (!removeIfPresent(htmlTmp) || !removeIfPresent(markerTmp)) return false;
  FsFile output;
  if (!Storage.openFileForWrite("TXT", htmlTmp, output)) return false;
  bool success = streamToHtml(output);
  output.flush();
  const size_t htmlSize = output.size();
  success = success && output.getError() == 0 && htmlSize > 0 && htmlSize <= UINT32_MAX;
  success = output.close() && success;
  uint64_t completedFingerprint = 0;
  success = success && fingerprintSource(filepath, fileSize, completedFingerprint) &&
            completedFingerprint == fingerprint;
  if (!success) {
    Storage.remove(htmlTmp.c_str());
    LOG_ERR("TXT", "XHTML conversion failed or source changed: %s", filepath.c_str());
    return false;
  }

  // Retire the old validity marker before replacing any derived data.
  if (!removeIfPresent(markerPath)) return false;
  const std::string sections = readerCachePath + "/sections";
  if (Storage.exists(sections.c_str()) && !Storage.removeDir(sections.c_str())) return false;
  if (!removeIfPresent(readerCachePath + "/book.bin") ||
      !removeIfPresent(readerCachePath + "/.full_cache_complete") ||
      !removeIfPresent(htmlPath) || !Storage.rename(htmlTmp.c_str(), htmlPath.c_str())) return false;

  FsFile marker;
  if (!Storage.openFileForWrite("TXT", markerTmp, marker)) return false;
  const uint32_t storedSize = static_cast<uint32_t>(htmlSize);
  success = marker.write(&CACHE_MAGIC, sizeof(CACHE_MAGIC)) == sizeof(CACHE_MAGIC) &&
            marker.write(&CACHE_VERSION, sizeof(CACHE_VERSION)) == sizeof(CACHE_VERSION) &&
            marker.write(&fingerprint, sizeof(fingerprint)) == sizeof(fingerprint) &&
            marker.write(&storedSize, sizeof(storedSize)) == sizeof(storedSize);
  marker.flush();
  success = success && marker.getError() == 0;
  success = marker.close() && success;
  if (!success || !Storage.rename(markerTmp.c_str(), markerPath.c_str())) {
    Storage.remove(markerTmp.c_str());
    return false;
  }
  LOG_INF("TXT", "Cached XHTML: source=%lu html=%lu", static_cast<unsigned long>(fileSize),
          static_cast<unsigned long>(htmlSize));
  return true;
}
