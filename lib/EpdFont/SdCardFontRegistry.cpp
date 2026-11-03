#include "SdCardFontRegistry.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

// --- SdCardFontFamilyInfo helpers ---

const SdCardFontFileInfo* SdCardFontFamilyInfo::findFile(uint8_t size, uint8_t style) const {
  for (const auto& f : files) {
    if (f.pointSize == size && f.style == style) return &f;
  }
  return nullptr;
}

bool SdCardFontFamilyInfo::hasSize(uint8_t size) const {
  for (const auto& f : files) {
    if (f.pointSize == size) return true;
  }
  return false;
}

std::vector<uint8_t> SdCardFontFamilyInfo::availableSizes() const {
  std::vector<uint8_t> sizes;
  for (const auto& f : files) {
    bool found = false;
    for (uint8_t s : sizes) {
      if (s == f.pointSize) {
        found = true;
        break;
      }
    }
    if (!found) sizes.push_back(f.pointSize);
  }
  std::sort(sizes.begin(), sizes.end());
  return sizes;
}

// --- SdCardFontRegistry ---

bool SdCardFontRegistry::parseFilename(const char* filename, uint8_t& size, uint8_t& style) {
  // V4 naming: <name>_<size>.cpfont (e.g. Bookerly-SD_14.cpfont)
  const char* ext = strstr(filename, ".cpfont");
  if (!ext) return false;

  size_t baseLen = ext - filename;
  if (baseLen == 0 || baseLen > 127) return false;

  char base[128];
  memcpy(base, filename, baseLen);
  base[baseLen] = '\0';

  char* lastUnderscore = strrchr(base, '_');
  if (!lastUnderscore || lastUnderscore == base) return false;

  const char* sizeStr = lastUnderscore + 1;
  char* endPtr;
  long sizeVal = strtol(sizeStr, &endPtr, 10);
  if (endPtr == sizeStr || *endPtr != '\0' || sizeVal < 1 || sizeVal > 255) return false;
  size = static_cast<uint8_t>(sizeVal);
  style = 0;
  return true;
}

void SdCardFontRegistry::scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family) {
  FsFile dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) return;

  char nameBuffer[128];
  while (true) {
    FsFile entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }

    entry.getName(nameBuffer, sizeof(nameBuffer));
    entry.close();

    // Skip macOS resource fork files (._*) and other hidden files
    if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;

    uint8_t size, style;
    if (!parseFilename(nameBuffer, size, style)) continue;

    SdCardFontFileInfo info;
    info.path = std::string(dirPath) + "/" + nameBuffer;
    info.pointSize = size;
    info.style = style;
    family.files.push_back(std::move(info));
  }
  dir.close();
}

bool SdCardFontRegistry::discover() {
  families_.clear();
  // Most cards contain only a few families. Let capacity follow the families
  // found instead of retaining 128 slots for the lifetime of the reader.

  // Prefer the hidden install directory, then the former public directory,
  // then the original CrossPoint metadata location.
  const char* fontDirs[] = {FONTS_DIR, PUBLIC_FONTS_DIR, LEGACY_FONTS_DIR};
  bool foundDirectory = false;
  for (const char* fontDir : fontDirs) {
    FsFile root = Storage.open(fontDir);
    if (!root) continue;
    if (!root.isDirectory()) {
      LOG_ERR("SDREG", "Fonts path is not a directory: %s", fontDir);
      root.close();
      continue;
    }
    foundDirectory = true;

    char nameBuffer[128];
    while (true) {
      FsFile entry = root.openNextFile();
      if (!entry) break;
      if (entry.isDirectory()) {
        // Subdirectory = font family
        entry.getName(nameBuffer, sizeof(nameBuffer));
        entry.close();

        // Skip hidden/system directories (macOS ._*, .Trashes, etc.)
        if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;

        // Directories are scanned in priority order, so the first matching
        // family wins when it exists in more than one location.
        if (findFamily(nameBuffer)) continue;

        SdCardFontFamilyInfo family;
        family.name = nameBuffer;
        family.path = std::string(fontDir) + "/" + nameBuffer;
        scanDirectory(family.path.c_str(), family);

        if (!family.files.empty()) {
          families_.push_back(std::move(family));
          LOG_DBG("SDREG", "Found family: %s (%d files)", families_.back().name.c_str(),
                  static_cast<int>(families_.back().files.size()));
        }
      } else {
        entry.close();
      }
    }
    root.close();
  }

  if (!foundDirectory) {
    LOG_ERR("SDREG", "Fonts directories not found: %s, %s, or %s", FONTS_DIR, PUBLIC_FONTS_DIR, LEGACY_FONTS_DIR);
    return false;
  }

  // Sort families alphabetically
  std::sort(families_.begin(), families_.end(),
            [](const SdCardFontFamilyInfo& a, const SdCardFontFamilyInfo& b) { return a.name < b.name; });

  // Cap at MAX_SD_FAMILIES
  if (static_cast<int>(families_.size()) > MAX_SD_FAMILIES) {
    families_.resize(MAX_SD_FAMILIES);
  }

  LOG_DBG("SDREG", "Discovery complete: %d families", static_cast<int>(families_.size()));
  return !families_.empty();
}

const SdCardFontFamilyInfo* SdCardFontRegistry::findFamily(const std::string& name) const {
  for (const auto& f : families_) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

int SdCardFontRegistry::getFamilyIndex(const std::string& name) const {
  for (int i = 0; i < static_cast<int>(families_.size()); i++) {
    if (families_[i].name == name) return i;
  }
  return -1;
}
