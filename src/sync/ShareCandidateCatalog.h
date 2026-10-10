#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace yomuka::sync {
// Same bit assignments as SnapshotExchange's UnitMask, without its EPUB dependencies.
enum SavedUnit : uint8_t { SavedPosition = 1, SavedBookmarks = 2, SavedSettings = 4, SavedHistory = 8 };
struct ShareCandidate {
  uint64_t bookId = 0;
  uint8_t units = 0, errors = 0, emptyUnits = 0;
  uint8_t locationPriority = 255;
  uint16_t historyRank = std::numeric_limits<uint16_t>::max();
  std::string path, title;
  bool resolved() const { return locationPriority != 255; }
  bool deletionOnly() const { return units == SavedHistory && emptyUnits == SavedHistory && errors == 0; }
};

// Pure discovery/ordering rules. No filesystem operations or implicit migration.
class ShareCandidateCatalog {
  std::vector<ShareCandidate> entries;
  ShareCandidate& obtain(uint64_t id);

 public:
  const std::vector<ShareCandidate>& books() const { return entries; }
  static bool parseBookId(const char* key, uint64_t& id);
  void observe(uint64_t id, uint8_t unit, bool valid, bool empty = false);
  bool collectHistory(const JsonDocument& document, size_t& legacyCount);
  bool collectSettings(const JsonDocument& document);
  bool collectPaths(const JsonDocument& document);
  void resolve(uint64_t id, const std::string& path, uint8_t priority);
  void finish();
  // Excludes unresolved history-deletion-only records from the user-facing count.
  size_t missingCount() const;
  // Includes deletion records so an explicit SD search can still find them.
  size_t unresolvedCount() const;
};
}  // namespace yomuka::sync
