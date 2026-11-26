#pragma once

#include <cstdint>
#include <string>

namespace TxtProgressMigration {

enum class Result { Unchanged, Migrated, NoPosition, Failed };

// Restore an approximate source position after TXT reflow. Existing new-format
// progress always wins. The legacy progress and page index are retained.
Result migrate(const std::string& filepath, const std::string& cacheRoot,
               const std::string& readerCachePath, uint16_t spineCount);

}  // namespace TxtProgressMigration
