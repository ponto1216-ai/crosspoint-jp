#pragma once

#include <cstdint>
#include <vector>

namespace yomuka::sync {
// SD-local discovery preference. Reading history and shared snapshots are unchanged.
class ShareSearchExclusions {
 public:
  static bool load(std::vector<uint64_t>& ids);
  static bool save(const std::vector<uint64_t>& ids);
};
}  // namespace yomuka::sync
