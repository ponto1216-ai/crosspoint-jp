#include "BookIdentity.h"

#include <ArduinoJson.h>
#include <HalStorage.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace BookIdentity {
namespace {

constexpr char kIdentityPath[] = "/.crosspoint/book-identity-paths.json";
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

bool loadDocument(JsonDocument& document) {
  if (!Storage.exists(kIdentityPath)) {
    document["formatVersion"] = kFormatVersion;
    document["paths"].to<JsonObject>();
    return true;
  }
  const String json = Storage.readFile(kIdentityPath);
  return !json.isEmpty() && !deserializeJson(document, json) && (document["formatVersion"] | 0) == kFormatVersion &&
         document["paths"].is<JsonObject>();
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

}  // namespace BookIdentity
