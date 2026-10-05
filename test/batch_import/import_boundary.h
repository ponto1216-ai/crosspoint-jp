#pragma once
#include <cstring>
#include <memory>

#include "sync/ReceivedSnapshotImport.h"
#include "sync/SnapshotBookProbe.h"
#include "sync/SnapshotExchange.h"
class GfxRenderer {};
struct RenderLock {
  ~RenderLock() {}
};
struct HalPowerManager {
  struct Lock {
    ~Lock() {}
  };
};
inline bool settingsReady = true, layoutReady = true, fontsReady = true;
inline bool fakeLoadSettings() { return settingsReady; }
struct FakeFontSystem {
  void releaseLoadedFamily(GfxRenderer&) {};
  FakeFontSystem& registry() { return *this; }
  void* findFamily(const char*) { return fontsReady ? this : nullptr; }
};
inline FakeFontSystem sdFontSystem;
inline void invalidateBookListStatusIndexEntry(const std::string&, const char*) {}
namespace BookDataPath {
inline bool ensureDirectory(uint64_t) { return true; }
}  // namespace BookDataPath
class Epub {
  std::string path;

 public:
  Epub(const std::string& p, const char*) : path(p) {}
  bool getSourceFingerprint(uint64_t* id) {
    if (!Storage.exists(path.c_str())) return false;
    *id = std::stoull(*Storage.files[path], nullptr, 16);
    return true;
  }
  bool load() { return true; }
  unsigned getSpineItemsCount() { return 2; }
  std::string getTitle() { return "received"; }
  std::string getAuthor() { return "author"; }
};
namespace yomuka::sync {
struct SnapshotPositionProjector {
  GfxRenderer& renderer;
  std::shared_ptr<Epub> epub;
  ExchangeBook book;
  std::function<bool()> cancel;
  bool renderLocked;
  static bool project(const Progress& p, const BookReaderSettings::Override*, Progress& out, void* context) {
    auto& self = *static_cast<SnapshotPositionProjector*>(context);
    if (!layoutReady || (self.cancel && self.cancel())) return false;
    out = p;
    out.chapterPage = 2;
    out.chapterPageCount = 10;
    return true;
  }
};
}  // namespace yomuka::sync
