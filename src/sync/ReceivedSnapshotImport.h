#pragma once
#include "BatchImport.h"
class GfxRenderer;
namespace yomuka::sync {
ImportOutcome importReceivedSnapshot(const ReceivedBook&, GfxRenderer&, const std::function<bool()>&);
}
