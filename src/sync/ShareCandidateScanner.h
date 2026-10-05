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
  void aggregate(const char* path, Phase source);
  void inspectBookData(uint64_t id);
  void checkEpub(const std::string& path, uint64_t expectedId = 0);
  void complete();

 public:
  size_t checkedFiles = 0, warnings = 0, legacyCount = 0, missingBooks = 0;
  bool step();  // true when discovery is complete
  bool done() const { return phase == Phase::Done; }
  const std::vector<ShareCandidate>& books() const { return catalog.books(); }
};
}  // namespace yomuka::sync
