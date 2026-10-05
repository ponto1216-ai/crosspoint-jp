#include "ReceivedSnapshotImport.h"

#include <HalPowerManager.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "ReadingStatusHelper.h"
#include "SdCardFontGlobals.h"
#include "SnapshotBookProbe.h"
#include "SnapshotPositionProjector.h"
#include "SnapshotValidation.h"
#include "activities/RenderLock.h"
#include "util/BookDataPath.h"
namespace yomuka::sync {
namespace {
bool fontAvailable(const char* family, const char* sdName, void*) {
  return sdName && *sdName ? sdFontSystem.registry().findFamily(sdName) != nullptr
                           : family && std::strcmp(family, "noto-sans") == 0;
}
}  // namespace
ImportOutcome importReceivedSnapshot(const ReceivedBook& received, GfxRenderer& renderer,
                                     const std::function<bool()>& cancel) {
  RenderLock render;
  HalPowerManager::Lock power;
  bool cancelled = false;
  const auto stop = [&] {
    if (!cancelled && cancel) cancelled = cancel();
    return cancelled;
  };
  if (stop()) return {ImportResult::Cancelled};
  if (!READING_HISTORY.endSession() || !SETTINGS.loadFromFile())
    return {ImportResult::RestartRequired, ImportError::StorageIo};
  sdFontSystem.releaseLoadedFamily(renderer);
  ExchangeBook book{received.id, 0, received.path, received.title, ""};
  if (!probeSnapshotBook(book.path, book.id, book.spines, stop))
    return stop() ? ImportOutcome{ImportResult::Cancelled} : ImportOutcome{ImportResult::Failed, ImportError::Source};
  JsonDocument snapshot;
  if (readSnapshotForBook(received.file, book, snapshot) != ExchangeError::None)
    return {ImportResult::Failed, ImportError::File};
  if (!snapshotSettingsAvailable(snapshot, fontAvailable, nullptr)) return {ImportResult::Failed, ImportError::Font};
  auto epub = std::make_shared<Epub>(book.path, "/.crosspoint");
  uint64_t id = 0;
  if (!epub->getSourceFingerprint(&id) || id != book.id || !epub->load() || epub->getSpineItemsCount() != book.spines)
    return {ImportResult::Failed, ImportError::Source};
  book.title = epub->getTitle();
  book.author = epub->getAuthor();
  uint8_t units = 0;
  constexpr const char* names[] = {"progress", "bookmarks", "readerSettings", "history"};
  for (unsigned i = 0; i < 4; ++i)
    if (!snapshot["units"][names[i]].isUnbound()) units |= 1 << i;
  SnapshotPositionProjector projector{renderer, epub, book, stop, true};
  std::vector<PreparedFile> files;
  auto preparation = prepareSnapshotImport(book, snapshot, units, READING_HISTORY, SnapshotPositionProjector::project,
                                           &projector, files);
  if (stop()) return {ImportResult::Cancelled};
  if (preparation != ExchangeError::None)
    return {ImportResult::Failed, preparation == ExchangeError::Layout ? ImportError::Layout : ImportError::StorageIo};
  if (!BookDataPath::ensureDirectory(book.id)) return {ImportResult::Failed, ImportError::StorageIo};
  // From here a book transaction finishes without cancellation. Never expose a partial import.
  const auto result = applyPreparedFiles(files);
  if (result == TransactionResult::RecoveryRequired) return {ImportResult::RestartRequired, ImportError::Recovery};
  if (result != TransactionResult::Committed && result != TransactionResult::CleanupPending)
    return {ImportResult::Failed, ImportError::StorageIo};
  invalidateBookListStatusIndexEntry(book.path, "/.crosspoint");
  if (!READING_HISTORY.reloadAfterSync()) return {ImportResult::RestartRequired, ImportError::Reload, true};
  if (result == TransactionResult::CleanupPending) return {ImportResult::RestartRequired, ImportError::Recovery, true};
  return {ImportResult::Imported, ImportError::None, true};
}
}  // namespace yomuka::sync
