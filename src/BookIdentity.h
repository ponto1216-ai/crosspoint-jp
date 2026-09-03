#pragma once

#include <cstdint>
#include <string>

// Tracks the latest EPUB archive fingerprint seen at each normalized SD-card
// path. The path is only an update-detection hint: canonical reader data stays
// keyed by the archive fingerprint.
namespace BookIdentity {

bool getLastArchiveId(const std::string& path, uint64_t& bookId);
// One-time bridge for installations that predate the path identity index.
// The cache marker is read before Epub::load() invalidates a changed cache.
bool getCachedArchiveId(const std::string& cachePath, uint64_t& bookId);
bool recordArchiveId(const std::string& path, uint64_t bookId);
// Moves the path-only update-detection hint after a file-browser rename.
// Canonical per-book data remains keyed by the unchanged archive fingerprint.
bool movePath(const std::string& oldPath, const std::string& newPath);
// Stores the original locator for a file moved to the in-app archive.
bool recordArchiveLocation(const std::string& originalPath, const std::string& archivedPath);
bool getArchiveRestorePath(const std::string& archivedPath, std::string& originalPath);
bool clearArchiveLocation(const std::string& archivedPath);

}  // namespace BookIdentity
