#include "DirectShareProtocol.h"

#include <HalStorage.h>

#include <cctype>
#include <cstdio>
#include <cstring>

#include "SnapshotUpload.h"
#include "StorageIo.h"

namespace yomuka::sync {
namespace {
bool hex(const std::string& text, bool nonzero = false) {
  if (text.size() != 16) return false;
  bool any = false;
  for (char c : text) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    any |= c != '0';
  }
  return !nonzero || any;
}
std::string trim(std::string text) {
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
  const auto start = text.find_first_not_of(" \t");
  return start == std::string::npos ? "" : text.substr(start);
}
}  // namespace
bool parseDirectHeader(const std::string& text, bool response, DirectHeader& output) {
  if (text.size() > 2048 || text.size() < 4 || text.substr(text.size() - 4) != "\r\n\r\n") return false;
  for (unsigned char c : text)
    if ((c < 32 && c != '\r' && c != '\n' && c != '\t') || c > 126) return false;
  const auto lineEnd = text.find("\r\n");
  const std::string first = text.substr(0, lineEnd);
  DirectHeader parsed;
  if (response) {
    if (first.size() < 12 || (first.substr(0, 9) != "HTTP/1.1 " && first.substr(0, 9) != "HTTP/1.0 ")) return false;
    for (size_t i = 9; i < 12; ++i) {
      if (first[i] < '0' || first[i] > '9') return false;
      parsed.status = parsed.status * 10 + first[i] - '0';
    }
    if (first.size() > 12 && first[12] != ' ') return false;
  } else {
    const auto space = first.find(' '), last = first.rfind(' ');
    if (space == std::string::npos || space == last || first.substr(last + 1) != "HTTP/1.1") return false;
    parsed.method = first.substr(0, space);
    parsed.path = first.substr(space + 1, last - space - 1);
    if (!((parsed.method == "GET" && parsed.path == "/yomuka/hello") ||
          (parsed.method == "POST" && parsed.path == "/yomuka/snapshot")))
      return false;
  }
  bool lengthSeen = false, sessionSeen = false, typeSeen = false;
  size_t at = lineEnd + 2;
  while (at < text.size() - 2) {
    const auto end = text.find("\r\n", at), colon = text.find(':', at);
    if (end == std::string::npos || colon == std::string::npos || colon >= end || colon == at) return false;
    std::string name = text.substr(at, colon - at);
    for (char& c : name) {
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return false;
      c = std::tolower(static_cast<unsigned char>(c));
    }
    const auto value = trim(text.substr(colon + 1, end - colon - 1));
    if (name == "transfer-encoding" || name == "expect") return false;
    if (name == "content-length") {
      if (lengthSeen || value.empty()) return false;
      lengthSeen = true;
      for (char c : value) {
        if (c < '0' || c > '9') return false;
        parsed.length = parsed.length * 10 + c - '0';
        if (parsed.length > (response ? 512 : DirectSnapshotLimit)) return false;
      }
    } else if (name == "x-yomuka-session") {
      if (sessionSeen || !hex(value)) return false;
      sessionSeen = true;
      parsed.session = value;
    } else if (name == "content-type") {
      if (typeSeen || value != "application/json") return false;
      typeSeen = true;
    }
    at = end + 2;
  }
  if (response && (!lengthSeen || !parsed.length)) return false;
  if (!response && parsed.method == "POST" && (!lengthSeen || !parsed.length || !typeSeen || !sessionSeen))
    return false;
  if (!response && parsed.method == "GET" && parsed.length) return false;
  output = std::move(parsed);
  return true;
}
std::string directDigest(const uint8_t* bytes, size_t size) {
  uint64_t hash = 14695981039346656037ULL;
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  char text[17];
  snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
  return text;
}
bool directHello(const JsonDocument& doc, std::string& device, std::string& session) {
  if (doc["protocol"] != "yomuka-direct" || !doc["version"].is<unsigned>() || doc["version"] != 1 ||
      !doc["device"].is<const char*>() || !doc["session"].is<const char*>())
    return false;
  std::string label = doc["device"].as<const char*>(), token = doc["session"].as<const char*>();
  if (label.empty() || label.size() > 48 || !hex(token)) return false;
  for (unsigned char c : label)
    if (c < 32 || c > 126) return false;
  device = std::move(label);
  session = std::move(token);
  return true;
}
bool directAccepted(const JsonDocument& doc, const std::string& id, const std::string& digest, size_t size) {
  return doc["accepted"].is<bool>() && doc["accepted"] == true && doc["bookId"] == id && doc["digest"] == digest &&
         doc["bytes"].is<size_t>() && doc["bytes"] == size;
}
void DirectSnapshot::clear() {
  bytes.reset();
  expected = used = 0;
}
bool DirectSnapshot::begin(size_t size, size_t free, size_t largest) {
  clear();
  if (!size || !SnapshotUpload::memoryAvailable(size, free, largest)) return false;
  bytes.reset(new (std::nothrow) uint8_t[size]);
  if (!bytes) return false;
  expected = size;
  return true;
}
bool DirectSnapshot::append(const uint8_t* data, size_t count) {
  if (!bytes || (!data && count) || count > expected - used) {
    clear();
    return false;
  }
  if (count) std::memcpy(bytes.get() + used, data, count);
  used += count;
  return true;
}
bool DirectSnapshot::publish(JsonDocument& receipt, size_t free, size_t largest) {
  if (!bytes || !used || used != expected || !SnapshotUpload::memoryAvailable(used, free, largest)) return false;
  JsonDocument doc;
  if (parseSnapshot(bytes.get(), used, doc) != SnapshotError::None) return false;
  const std::string id = doc["bookId"].as<const char*>();
  if (!hex(id, true)) return false;
  const auto digest = directDigest(bytes.get(), used);
  // Defer EPUB/spine matching to the existing device-side import confirmation.
  // This directory is an inbox, never the reader's live data directory.
  if (!Storage.ensureDirectoryExists("/YomukaSync") || !writeBytes("/YomukaSync/" + id + ".json", bytes.get(), used))
    return false;
  receipt["accepted"] = true;
  receipt["bookId"] = id;
  receipt["digest"] = digest;
  receipt["bytes"] = used;
  clear();
  return true;
}
}  // namespace yomuka::sync
