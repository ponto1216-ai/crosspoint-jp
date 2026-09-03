#include "BookIdentity.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace BookIdentity {
namespace {

constexpr char kIdentityPath[] = "/.crosspoint/book-identity-paths.json";
constexpr char kArchiveLocationDirectory[] = "/.crosspoint/archive-locations";
constexpr uint8_t kFormatVersion = 1;
constexpr char kCacheFingerprintFile[] = "/source.fingerprint";
constexpr uint8_t kCacheFingerprintVersion = 1;

std::string normalizePath(const std::string& path) {
  std::string normalized;
  normalized.reserve(path.size() + 1);
  if (path.empty() || path.front() != '/') normalized.push_back('/');
  bool previousWasSlash = false;
  for (char c : path) {
    if (c == '\\') c = '/';
    if (c == '/') {
      if (previousWasSlash) continue;
      previousWasSlash = true;
    } else {
      previousWasSlash = false;
    }
    normalized.push_back(c);
  }
  return normalized;
}

void fingerprintKey(const uint64_t bookId, char (&out)[17]) {
  snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(bookId));
}

bool parseFingerprint(const char* value, uint64_t& bookId) {
  if (!value || strlen(value) != 16) return false;
  char* end = nullptr;
  const unsigned long long parsed = strtoull(value, &end, 16);
  if (!end || *end != '\0') return false;
  bookId = static_cast<uint64_t>(parsed);
  return true;
}

std::string archiveLocationPath(const std::string& archivedPath) {
  return std::string(kArchiveLocationDirectory) + "/" + std::to_string(std::hash<std::string>{}(archivedPath)) + ".path";
}

bool loadDocument(JsonDocument& document) {
  if (!Storage.exists(kIdentityPath)) {
    document["formatVersion"] = kFormatVersion;
    document["paths"].to<JsonObject>();
    document["archives"].to<JsonObject>();
    return true;
  }
  const String json = Storage.readFile(kIdentityPath);
  if (json.isEmpty() || deserializeJson(document, json) || (document["formatVersion"] | 0) != kFormatVersion ||
      !document["paths"].is<JsonObject>()) {
    return false;
  }
  if (!document["archives"].is<JsonObject>()) document["archives"].to<JsonObject>();
  return true;
}

}  // namespace

bool getLastArchiveId(const std::string& path, uint64_t& bookId) {
  if (!Storage.ready()) return false;
  JsonDocument document;
  if (!loadDocument(document)) return false;
  const std::string normalizedPath = normalizePath(path);
  return parseFingerprint(document["paths"][normalizedPath].as<const char*>(), bookId);
}

bool getCachedArchiveId(const std::string& cachePath, uint64_t& bookId) {
  FsFile marker;
  if (!Storage.openFileForRead("BID", cachePath + kCacheFingerprintFile, marker)) return false;
  uint8_t version = 0;
  const bool valid = marker.read(&version, sizeof(version)) == sizeof(version) &&
                     marker.read(&bookId, sizeof(bookId)) == sizeof(bookId) && marker.available() == 0 &&
                     version == kCacheFingerprintVersion;
  marker.close();
  return valid;
}

bool recordArchiveId(const std::string& path, const uint64_t bookId) {
  if (!Storage.ready() || !Storage.ensureDirectoryExists("/.crosspoint")) return false;
  JsonDocument document;
  if (!loadDocument(document)) return false;
  char key[17];
  fingerprintKey(bookId, key);
  const std::string normalizedPath = normalizePath(path);
  document["paths"][normalizedPath] = key;
  String json;
  serializeJson(document, json);
  return Storage.writeFile(kIdentityPath, json);
}

bool movePath(const std::string& oldPath, const std::string& newPath) {
  if (!Storage.ready()) return false;
  JsonDocument document;
  if (!loadDocument(document)) return false;
  const std::string normalizedOldPath = normalizePath(oldPath);
  const std::string normalizedNewPath = normalizePath(newPath);
  uint64_t bookId = 0;
  if (!parseFingerprint(document["paths"][normalizedOldPath].as<const char*>(), bookId)) return false;

  char key[17];
  fingerprintKey(bookId, key);
  document["paths"][normalizedNewPath] = key;
  document["paths"].as<JsonObject>().remove(normalizedOldPath.c_str());
  String json;
  serializeJson(document, json);
  return Storage.writeFile(kIdentityPath, json);
}

bool recordArchiveLocation(const std::string& originalPath, const std::string& archivedPath) {
  if (!Storage.ready() || !Storage.ensureDirectoryExists(kArchiveLocationDirectory)) return false;
  const std::string normalizedOriginalPath = normalizePath(originalPath);
  const std::string normalizedArchivedPath = normalizePath(archivedPath);
  const bool saved = Storage.writeFile(archiveLocationPath(normalizedArchivedPath).c_str(), normalizedOriginalPath.c_str());
  if (saved) {
    LOG_INF("BID", "Recorded archive location: %s -> %s", normalizedOriginalPath.c_str(), normalizedArchivedPath.c_str());
  } else {
    LOG_ERR("BID", "Could not record archive location: %s", normalizedArchivedPath.c_str());
  }
  return saved;
}

bool getArchiveRestorePath(const std::string& archivedPath, std::string& originalPath) {
  originalPath.clear();
  if (!Storage.ready()) return false;
  const std::string normalizedArchivedPath = normalizePath(archivedPath);
  const String storedPath = Storage.readFile(archiveLocationPath(normalizedArchivedPath).c_str());
  if (storedPath.isEmpty()) return false;
  originalPath = storedPath.c_str();
  return !originalPath.empty();
}

bool clearArchiveLocation(const std::string& archivedPath) {
  if (!Storage.ready()) return false;
  const std::string normalizedArchivedPath = normalizePath(archivedPath);
  const std::string locationPath = archiveLocationPath(normalizedArchivedPath);
  return !Storage.exists(locationPath.c_str()) || Storage.remove(locationPath.c_str());
}

}  // namespace BookIdentity
