#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace ProgressFile {

// Writes `len` bytes of reader progress to a path without
// ever leaving the canonical file half-written.
//
// The bytes go to a temporary `progress.bin.tmp` first; only once that is fully
// written and closed is it renamed over progress.bin. An interrupted write
// (power loss or a crash mid-SPI) therefore damages only the throwaway temp file.
// Previously a truncate-in-place write that was cut short left progress.bin with
// a broken FAT cluster chain that the firmware could neither rewrite nor clear,
// stranding the book on an old page (issue #2275).
//
// This is crash-safe, not metadata-atomic: on FAT the replace is remove + rename,
// two separate directory operations, so a crash between them can leave neither
// file -- which simply reads as "no saved progress" on next launch, never a
// corrupt or unclearable file. The point is that progress.bin is never torn.
//
// Note: this prevents corruption on a healthy card going forward. It cannot
// repair an already-corrupted progress.bin -- removing the stale file may itself
// fail at the FAT level, in which case recovery still requires fsck on a host.
//
// Returns true only if the new progress.bin is fully in place.
inline bool writeAtomicPath(const std::string& finalPath, const uint8_t* data, size_t len) {
  const std::string tmpPath = finalPath + ".tmp";

  {
    HalFile f;
    if (!Storage.openFileForWrite("PRG", tmpPath, f)) {
      LOG_ERR("PRG", "Could not open temp progress file for write: %s", tmpPath.c_str());
      return false;
    }
    const size_t written = f.write(data, len);
    if (written != len) {
      LOG_ERR("PRG", "Short write saving progress to %s: %u/%u bytes", tmpPath.c_str(), (unsigned)written,
              (unsigned)len);
      return false;
    }
    f.flush();
    // f (the temp file) is closed at scope exit (DESTRUCTOR_CLOSES_FILE=1) before
    // the rename below -- SdFat must not rename a path that still has an open FsFile.
  }

  // SdFat's rename does not overwrite an existing destination, so drop the old
  // canonical file first. The brief window where neither file exists reads as
  // "no saved progress" on next launch -- never a corrupt, unclearable file.
  Storage.remove(finalPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR("PRG", "Failed to rename temp progress into place: %s", finalPath.c_str());
    return false;
  }
  return true;
}

// Legacy TXT/XTC callers still store progress in their path-keyed cache
// directories. EPUB passes its fingerprint-keyed canonical file path above.
inline bool writeAtomic(const std::string& cachePath, const uint8_t* data, size_t len) {
  return writeAtomicPath(cachePath + "/progress.bin", data, len);
}

// EPUB progress historically used 4, 6, and 7 byte payloads. Keep all three
// readable during the fingerprint-path migration; callers use the first six
// bytes for the resume position and retain byte seven as the finished marker.
inline size_t readLegacyCompatible(const std::string& path, uint8_t (&data)[7]) {
  FsFile file;
  if (!Storage.openFileForRead("PRG", path, file)) return 0;
  const int bytesRead = file.read(data, sizeof(data));
  file.close();
  return bytesRead == 4 || bytesRead == 6 || bytesRead == 7 ? static_cast<size_t>(bytesRead) : 0;
}

}  // namespace ProgressFile
