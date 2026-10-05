#pragma once
#include "ReceivedBookScanner.h"
namespace yomuka::sync {
// Read-only, bounded summaries. Never repairs backups, flushes stores or builds layout.
std::vector<std::string> compareReceivedBook(const ReceivedBook&);
std::string compareUpdateTimes(uint32_t local, uint32_t incoming);
}  // namespace yomuka::sync
