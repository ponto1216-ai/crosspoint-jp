#pragma once

#include <HalStorage.h>

#include "ShareCandidateCatalog.h"

namespace yomuka::sync {
// Advances one directory entry/file at a time so the activity remains cancellable.
// Uses only SD reads; never loads an EPUB, repairs files, or migrates reader data.
class ShareCandidateScanner {
  enum class Phase { History, Settings, BookData, Paths, KnownPaths, Search, Done };
  struct Directory {
    FsFile file;
    std::string path;
  };
  Phase phase = Phase::History;
  ShareCandidateCatalog catalog;
  FsFile bookDirectory;
  std::vector<Directory> directories;
  size_t knownIndex = 0;
  bool searchMissing;
  std::vector<uint64_t> excludedIds;
  std::vector<ShareCandidate> missingCandidates;
  struct UnresolvedRecord {
    uint64_t bookId;
    bool deletionOnly;
  };
  std::vector<UnresolvedRecord> unresolvedRecords;
  bool excluded(uint64_t id) const;
  size_t searchableCount() const;
  void aggregate(const char* path, Phase source);
  void inspectBookData(uint64_t id);
  void checkEpub(const std::string& path, uint64_t expectedId = 0);
  void complete();

 public:
  explicit ShareCandidateScanner(bool searchMissing = true) : searchMissing(searchMissing) {}
  size_t checkedFiles = 0, warnings = 0, legacyCount = 0, missingBooks = 0, unresolvedBooks = 0;
  void restart(bool searchMissing);
  void setExclusions(std::vector<uint64_t> ids);
  bool isExcluded(uint64_t id) const { return excluded(id); }
  size_t exclusionCount() const { return excludedIds.size(); }
  const std::vector<ShareCandidate>& unresolvedCandidates() const { return missingCandidates; }
  bool step();  // true when discovery is complete
  bool done() const { return phase == Phase::Done; }
  bool canSearchMissing() const { return done() && !searchMissing && unresolvedBooks != 0; }
  const std::vector<ShareCandidate>& books() const { return catalog.books(); }
};
}  // namespace yomuka::sync
