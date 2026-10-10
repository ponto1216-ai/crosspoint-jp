#include "ShareSearchExclusions.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "ShareCandidateCatalog.h"
#include "StorageIo.h"

namespace yomuka::sync {
namespace {
constexpr const char* Path = "/.crosspoint/share-search-exclusions.json";
}
bool ShareSearchExclusions::load(std::vector<uint64_t>& ids) {
  JsonDocument document;
  const auto status = readJson(Path, document, 65536);
  if (status == ReadStatus::Absent) {
    ids.clear();
    return true;
  }
  if (status != ReadStatus::Present || document["formatVersion"].as<int>() != 1 ||
      !document["bookIds"].is<JsonArrayConst>())
    return false;
  std::vector<uint64_t> parsed;
  for (JsonVariantConst value : document["bookIds"].as<JsonArrayConst>()) {
    uint64_t id = 0;
    if (!value.is<const char*>() || !ShareCandidateCatalog::parseBookId(value.as<const char*>(), id)) return false;
    parsed.push_back(id);
  }
  std::sort(parsed.begin(), parsed.end());
  if (std::adjacent_find(parsed.begin(), parsed.end()) != parsed.end()) return false;
  ids = std::move(parsed);
  return true;
}
bool ShareSearchExclusions::save(const std::vector<uint64_t>& ids) {
  JsonDocument document;
  document["formatVersion"] = 1;
  auto values = document["bookIds"].to<JsonArray>();
  for (uint64_t id : ids) {
    char key[17];
    std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(id));
    values.add(key);
  }
  return writeJson(Path, document);
}
}  // namespace yomuka::sync
