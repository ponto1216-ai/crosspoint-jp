#include "BookmarkStorage.h"

#include <HalStorage.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace yomuka {
namespace sync {
namespace {
bool decode(JsonObjectConst object, std::vector<BookmarkEntry>& value) {
  const auto entries = object["bookmarks"].as<JsonArrayConst>();
  if (entries.isNull() || entries.size() > 24) return false;
  std::vector<BookmarkEntry> candidate;
  candidate.reserve(entries.size());
  for (JsonVariantConst item : entries) {
    if (!item.is<JsonObjectConst>()) return false;
    if (!item["summary"].is<const char*>() || !item["percentage"].is<double>() || !item["spine"].is<uint16_t>() ||
        !item["pages"].is<uint16_t>() || !item["page"].is<uint16_t>())
      return false;
    BookmarkEntry bookmark;
    bookmark.summary = item["summary"].as<std::string>();
    const double ratio = item["percentage"].as<double>();
    bookmark.spineIndex = item["spine"].as<uint16_t>();
    bookmark.chapterPageCount = item["pages"].as<uint16_t>();
    bookmark.chapterPage = item["page"].as<uint16_t>();
    if (!std::isfinite(ratio) || ratio < 0 || ratio > 1 || bookmark.chapterPageCount == 0 ||
        bookmark.chapterPage >= bookmark.chapterPageCount || bookmark.summary.find('\0') != std::string::npos)
      return false;
    bookmark.percentage = static_cast<float>(ratio);
    candidate.push_back(std::move(bookmark));
  }
  value = std::move(candidate);
  return true;
}
void encode(JsonDocument& document, const std::vector<BookmarkEntry>& value) {
  JsonArray entries = document["bookmarks"].to<JsonArray>();
  for (const auto& bookmark : value) {
    auto entry = entries.add<JsonObject>();
    entry["summary"] = bookmark.summary;
    entry["percentage"] = bookmark.percentage;
    // Compare the number as it will actually be persisted, rather than an
    // unrounded float against a parsed decimal from the preceding save.
    char ratio[32];
    serializeJson(entry["percentage"], ratio, sizeof(ratio));
    entry["percentage"] = std::strtod(ratio, nullptr);
    entry["spine"] = bookmark.spineIndex;
    entry["pages"] = bookmark.chapterPageCount;
    entry["page"] = bookmark.chapterPage;
  }
}
bool sameBookmarks(JsonArrayConst previous, JsonArrayConst candidate) {
  if (previous.size() != candidate.size()) return false;
  for (size_t i = 0; i < previous.size(); ++i) {
    for (const char* field : {"summary", "spine", "pages", "page"}) {
      if (previous[i][field] != candidate[i][field]) return false;
    }
    // ArduinoJson's decimal parser and strtod need not round to the same last
    // binary bit. Compare their bounded serialized form, not binary doubles.
    char previousRatio[32], candidateRatio[32];
    const size_t a = serializeJson(previous[i]["percentage"], previousRatio, sizeof(previousRatio));
    const size_t b = serializeJson(candidate[i]["percentage"], candidateRatio, sizeof(candidateRatio));
    if (a != b || std::memcmp(previousRatio, candidateRatio, a) != 0) return false;
  }
  return true;
}
}  // namespace

bool decodeSavedBookmarks(JsonObjectConst object, std::vector<BookmarkEntry>& value, uint32_t& updatedAt) {
  uint32_t timestamp = 0;
  std::vector<BookmarkEntry> candidate;
  if (object.isNull() || !readUpdateTime(object, timestamp) || !decode(object, candidate)) return false;
  value = std::move(candidate);
  updatedAt = timestamp;
  return true;
}

ReadStatus readBookmarks(const std::string& path, std::vector<BookmarkEntry>& value, uint32_t& updatedAt) {
  HalStorage::StorageLock lock;
  JsonDocument document;
  const auto status = readJson(path, document, 65536);
  if (status != ReadStatus::Present) return status;
  uint32_t timestamp = 0;
  std::vector<BookmarkEntry> candidate;
  if (!document.is<JsonObject>() || !decodeSavedBookmarks(document.as<JsonObjectConst>(), candidate, timestamp))
    return ReadStatus::Corrupt;
  value = std::move(candidate);
  updatedAt = timestamp;
  return ReadStatus::Present;
}

bool saveBookmarks(const std::string& path, const std::vector<BookmarkEntry>& value, bool recordLocalChange) {
  HalStorage::StorageLock lock;
  JsonDocument candidate;
  encode(candidate, value);
  std::vector<BookmarkEntry> verified;
  if (candidate.overflowed() || !decode(candidate.as<JsonObjectConst>(), verified)) return false;
  JsonDocument previous;
  const auto status = readJson(path, previous, 65536);
  if (status != ReadStatus::Present && status != ReadStatus::Absent) return false;
  if (status == ReadStatus::Present) {
    uint32_t timestamp = 0;
    if (!previous.is<JsonObject>() || !readUpdateTime(previous.as<JsonObjectConst>(), timestamp) ||
        !decode(previous.as<JsonObjectConst>(), verified))
      return false;
    if (sameBookmarks(previous["bookmarks"].as<JsonArrayConst>(), candidate["bookmarks"].as<JsonArrayConst>()))
      return true;
  }
  candidate["updatedAt"] = recordLocalChange ? localUpdateTime() : 0;
  if (measureJson(candidate) > 65536) return false;
  return writeJson(path, candidate);
}
}  // namespace sync
}  // namespace yomuka
