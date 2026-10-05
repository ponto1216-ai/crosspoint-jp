#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace yomuka::sync {
// Read-only: verify current EPUB identity and count the same manifest-resolved
// spine entries as the reader, without loading an Epub or creating caches.
bool probeSnapshotBook(const std::string& path, uint64_t expectedId, uint32_t& spines,
                       const std::function<bool()>& cancel = {});
}  // namespace yomuka::sync
