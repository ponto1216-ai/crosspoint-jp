#pragma once

#include <cstdint>
#include <string>

// Canonical, user-owned EPUB data is keyed by the archive fingerprint rather
// than the SD-card path. Generated EPUB caches deliberately remain path-keyed.
namespace BookDataPath {

std::string getDirectory(uint64_t bookId);
std::string getProgressPath(uint64_t bookId);
std::string getBookmarkPath(uint64_t bookId);
bool ensureDirectory(uint64_t bookId);
// Copies only missing canonical files from a prior archive revision. Source
// files are retained for the 0.7.x compatibility period.
bool migrateArchiveData(uint64_t previousBookId, uint64_t currentBookId);

}  // namespace BookDataPath
