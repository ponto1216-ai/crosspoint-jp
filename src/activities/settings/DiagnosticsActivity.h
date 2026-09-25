#pragma once

#include <Epub.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"

class DiagnosticsActivity final : public Activity {
 public:
  explicit DiagnosticsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Diagnostics", renderer, mappedInput) {}
  DiagnosticsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> book,
                      int spineIndex, int pageIndex, int pageCount)
      : Activity("Diagnostics", renderer, mappedInput),
        book(std::move(book)),
        bookSpineIndex(spineIndex),
        bookPageIndex(pageIndex),
        bookPageCount(pageCount) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool supportsUiLandscape() const override { return true; }

 private:
  enum class Page : uint8_t { Overview, Logs, Details };
  enum class SaveResult : uint8_t { None, Saved, Failed };

  Page page = Page::Overview;
  SaveResult saveResult = SaveResult::None;
  bool sdReady = false;
  uint32_t freeHeap = 0;
  uint32_t maxAllocHeap = 0;
  uint32_t minFreeHeap = 0;
  uint32_t snapshotDurationMs = 0;
  uint64_t sdTotalBytes = 0;
  uint64_t sdUsedBytes = 0;
  uint64_t readingCacheBytes = 0;
  bool readingCacheSizeComplete = false;
  int cacheDirectoryCount = 0;
  std::string openBookType;
  uint32_t openBookSize = 0;
  std::string readerFont;
  bool readerVertical = false;
  uint8_t readerLineSpacing = 0;
  uint8_t readerImageRendering = 0;
  uint8_t readerBookStyle = 0;
  std::shared_ptr<Epub> book;
  bool hasActiveBook = false;
  Epub::CacheGenerationStatus bookCacheStatus = Epub::CacheGenerationStatus::NotGenerated;
  bool bookFingerprintAvailable = false;
  uint64_t bookFingerprint = 0;
  int bookSpineIndex = -1;
  int bookPageIndex = -1;
  int bookPageCount = 0;
  std::string recentLogs;
  std::vector<std::string> recentLogLines;
  std::string savedReportPath;

  void collectSnapshot();
  bool saveReport();
  void renderOverview(int x, int y, int contentWidth, int lineHeight);
  void renderLogs(int x, int y, int contentWidth, int lineHeight);
  void renderDetails(int x, int y, int contentWidth, int lineHeight);
};
