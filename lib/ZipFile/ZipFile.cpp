#include "ZipFile.h"

#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>

#include <algorithm>

struct ZipInflateCtx {
  InflateReader reader;  // Must be first — callback casts uzlib_uncomp* to ZipInflateCtx*
  FsFile* file = nullptr;
  size_t fileRemaining = 0;
  uint8_t* readBuf = nullptr;
  size_t readBufSize = 0;
};

namespace {
constexpr uint16_t ZIP_METHOD_STORED = 0;
constexpr uint16_t ZIP_METHOD_DEFLATED = 8;
constexpr size_t MAX_ONE_SHOT_STREAM_INFLATED_SIZE = 16 * 1024;

const char* zipMethodName(const uint16_t method) {
  switch (method) {
    case ZIP_METHOD_STORED:
      return "stored";
    case ZIP_METHOD_DEFLATED:
      return "deflated";
    default:
      return "unsupported";
  }
}

// RAII zip: opens the zip if not already open, closes on destruction only if
// it performed the open.  Removes the wasOpen/close boilerplate from every method.
class ScopedOpenClose final {
 public:
  [[nodiscard]] explicit ScopedOpenClose(ZipFile& zf) : zf(zf), needsClose(!zf.isOpen()) {
    if (needsClose) ok = zf.open();
  }
  ~ScopedOpenClose() {
    if (needsClose && ok) zf.close();
  }
  ScopedOpenClose(const ScopedOpenClose&) = delete;
  ScopedOpenClose& operator=(const ScopedOpenClose&) = delete;
  ScopedOpenClose(ScopedOpenClose&&) = delete;
  ScopedOpenClose& operator=(ScopedOpenClose&&) = delete;
  explicit operator bool() const { return ok || !needsClose; }

 private:
  ZipFile& zf;
  bool needsClose = false;
  bool ok = true;  // true when zip was already open (no open() call needed)
};

int zipReadCallback(uzlib_uncomp* uncomp) {
  auto* ctx = reinterpret_cast<ZipInflateCtx*>(uncomp);
  if (ctx->fileRemaining == 0) return -1;

  const size_t toRead = ctx->fileRemaining < ctx->readBufSize ? ctx->fileRemaining : ctx->readBufSize;
  const int bytesRead = ctx->file->read(ctx->readBuf, toRead);
  if (bytesRead <= 0) return -1;
  ctx->fileRemaining -= static_cast<size_t>(bytesRead);

  uncomp->source = ctx->readBuf + 1;
  uncomp->source_limit = ctx->readBuf + bytesRead;
  return ctx->readBuf[0];
}

uint8_t* allocateStreamChunk(const size_t requestedSize, size_t& allocatedSize) {
  constexpr size_t MIN_CHUNK_SIZE = 512;
  allocatedSize = 0;
  if (requestedSize == 0) return nullptr;

  size_t candidate = requestedSize;
  while (true) {
    if (auto* buffer = static_cast<uint8_t*>(malloc(candidate))) {
      allocatedSize = candidate;
      if (candidate != requestedSize) {
        LOG_DBG("ZIP", "Reduced stream chunk from %zu to %zu bytes (maxAlloc=%u)", requestedSize, candidate,
                ESP.getMaxAllocHeap());
      }
      return buffer;
    }
    if (candidate <= MIN_CHUNK_SIZE) break;
    candidate = std::max(MIN_CHUNK_SIZE, candidate / 2);
  }
  return nullptr;
}
}  // namespace

bool ZipFile::loadAllFileStatSlims() {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  file.seek(zipDetails.centralDirOffset);

  uint32_t sig;
  char itemName[256];
  fileStatSlimCache.clear();
  fileStatSlimCache.reserve(zipDetails.totalEntries);

  while (file.available()) {
    file.read(&sig, 4);
    if (sig != 0x02014b50) break;  // End of list

    FileStatSlim fileStat = {};

    file.seekCur(6);
    file.read(&fileStat.method, 2);
    file.seekCur(8);
    file.read(&fileStat.compressedSize, 4);
    file.read(&fileStat.uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    file.read(&fileStat.localHeaderOffset, 4);

    if (nameLen < sizeof(itemName)) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';
      fileStatSlimCache.emplace(itemName, fileStat);
    } else {
      // Skip over oversized entry names to avoid writing past fixed buffer.
      file.seekCur(nameLen);
    }

    // Skip the rest of this entry (extra field + comment)
    file.seekCur(m + k);
  }

  // Set cursor to start of central directory for sequential access
  lastCentralDirPos = zipDetails.centralDirOffset;
  lastCentralDirPosValid = true;

  return true;
}

bool ZipFile::loadFileStatSlim(const char* filename, FileStatSlim* fileStat) {
  if (!fileStatSlimCache.empty()) {
    const auto it = fileStatSlimCache.find(filename);
    if (it != fileStatSlimCache.end()) {
      *fileStat = it->second;
      return true;
    }
    return false;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  // Phase 1: Try scanning from cursor position first
  uint32_t startPos = lastCentralDirPosValid ? lastCentralDirPos : zipDetails.centralDirOffset;
  bool wrapped = false;
  bool found = false;

  file.seek(startPos);

  uint32_t sig;
  char itemName[256];

  while (true) {
    uint32_t entryStart = file.position();

    if (file.read(&sig, 4) != 4 || sig != 0x02014b50) {
      // End of central directory
      if (!wrapped && lastCentralDirPosValid && startPos != zipDetails.centralDirOffset) {
        // Wrap around to beginning
        file.seek(zipDetails.centralDirOffset);
        wrapped = true;
        continue;
      }
      break;
    }

    // If we've wrapped and reached our start position, stop
    if (wrapped && entryStart >= startPos) {
      break;
    }

    file.seekCur(6);
    file.read(&fileStat->method, 2);
    file.seekCur(8);
    file.read(&fileStat->compressedSize, 4);
    file.read(&fileStat->uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    file.read(&fileStat->localHeaderOffset, 4);

    if (nameLen < 256) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';

      if (strcmp(itemName, filename) == 0) {
        // Found it! Update cursor to next entry
        file.seekCur(m + k);
        lastCentralDirPos = file.position();
        lastCentralDirPosValid = true;
        found = true;
        break;
      }
    } else {
      // Name too long, skip it
      file.seekCur(nameLen);
    }

    // Skip extra field + comment
    file.seekCur(m + k);
  }

  return found;
}

long ZipFile::getDataOffset(const FileStatSlim& fileStat) {
  const ScopedOpenClose zip{*this};
  if (!zip) return -1;

  constexpr auto localHeaderSize = 30;

  uint8_t pLocalHeader[localHeaderSize];
  const uint64_t fileOffset = fileStat.localHeaderOffset;

  file.seek(fileOffset);
  const size_t read = file.read(pLocalHeader, localHeaderSize);

  if (read != localHeaderSize) {
    LOG_ERR("ZIP", "Something went wrong reading the local header");
    return -1;
  }

  if (pLocalHeader[0] + (pLocalHeader[1] << 8) + (pLocalHeader[2] << 16) + (pLocalHeader[3] << 24) !=
      0x04034b50 /* ZIP local file header signature */) {
    LOG_ERR("ZIP", "Not a valid zip file header");
    return -1;
  }

  const uint16_t filenameLength = pLocalHeader[26] + (pLocalHeader[27] << 8);
  const uint16_t extraOffset = pLocalHeader[28] + (pLocalHeader[29] << 8);
  return fileOffset + localHeaderSize + filenameLength + extraOffset;
}

bool ZipFile::loadZipDetails() {
  if (zipDetails.isSet) {
    return true;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  const size_t fileSize = file.size();
  if (fileSize < 22) {
    LOG_ERR("ZIP", "File too small to be a valid zip");
    return false;  // Minimum EOCD size is 22 bytes
  }

  // We scan the last 1KB (or the whole file if smaller) for the EOCD signature
  // 0x06054b50 is stored as 0x50, 0x4b, 0x05, 0x06 in little-endian
  const int scanRange = fileSize > 1024 ? 1024 : fileSize;
  const auto buffer = static_cast<uint8_t*>(malloc(scanRange));
  if (!buffer) {
    LOG_ERR("ZIP", "Failed to allocate memory for EOCD scan buffer");
    return false;
  }

  file.seek(fileSize - scanRange);
  file.read(buffer, scanRange);

  // Scan backwards for the signature
  int foundOffset = -1;
  for (int i = scanRange - 22; i >= 0; i--) {
    constexpr uint32_t signature = 0x06054b50;
    if (*reinterpret_cast<uint32_t*>(&buffer[i]) == signature) {
      foundOffset = i;
      break;
    }
  }

  if (foundOffset == -1) {
    LOG_ERR("ZIP", "EOCD signature not found in zip file");
    free(buffer);
    return false;
  }

  // Now extract the values we need from the EOCD record
  // Relative positions within EOCD:
  // Offset 10: Total number of entries (2 bytes)
  // Offset 12: Size of central directory (4 bytes)
  // Offset 16: Offset of start of central directory with respect to the starting disk number (4 bytes)
  zipDetails.totalEntries = *reinterpret_cast<uint16_t*>(&buffer[foundOffset + 10]);
  zipDetails.centralDirSize = *reinterpret_cast<uint32_t*>(&buffer[foundOffset + 12]);
  zipDetails.centralDirOffset = *reinterpret_cast<uint32_t*>(&buffer[foundOffset + 16]);
  zipDetails.isSet = true;

  free(buffer);
  return true;
}

bool ZipFile::open() {
  if (!Storage.openFileForRead("ZIP", filePath, file)) {
    return false;
  }
  return true;
}

bool ZipFile::close() {
  if (file) {
    // Explicit close() required: member variable persists beyond function scope
    file.close();
  }
  lastCentralDirPos = 0;
  lastCentralDirPosValid = false;
  return true;
}

bool ZipFile::getInflatedFileSize(const char* filename, size_t* size) {
  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) {
    return false;
  }

  *size = static_cast<size_t>(fileStat.uncompressedSize);
  return true;
}

bool ZipFile::getArchiveFingerprint(uint64_t* fingerprint) {
  if (!fingerprint) return false;

  const ScopedOpenClose zip{*this};
  if (!zip || !loadZipDetails()) return false;

  const size_t archiveSize = file.size();
  if (zipDetails.centralDirOffset > archiveSize ||
      zipDetails.centralDirSize > archiveSize - zipDetails.centralDirOffset) {
    LOG_ERR("ZIP", "Invalid central directory bounds: offset=%lu size=%lu archive=%zu",
            static_cast<unsigned long>(zipDetails.centralDirOffset),
            static_cast<unsigned long>(zipDetails.centralDirSize), archiveSize);
    return false;
  }

  uint64_t hash = 14695981039346656037ull;
  const auto hashByte = [&hash](const uint8_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  const auto hashValue = [&hashByte](uint64_t value, const size_t byteCount) {
    for (size_t i = 0; i < byteCount; ++i) {
      hashByte(static_cast<uint8_t>(value & 0xFF));
      value >>= 8;
    }
  };

  // Include the outer archive size and directory metadata so changes that move
  // or resize the directory also invalidate the cache.
  hashValue(archiveSize, sizeof(uint64_t));
  hashValue(zipDetails.centralDirOffset, sizeof(zipDetails.centralDirOffset));
  hashValue(zipDetails.centralDirSize, sizeof(zipDetails.centralDirSize));
  hashValue(zipDetails.totalEntries, sizeof(zipDetails.totalEntries));

  file.seek(zipDetails.centralDirOffset);
  uint8_t buffer[512];
  uint32_t remaining = zipDetails.centralDirSize;
  while (remaining > 0) {
    const size_t chunkSize = std::min<size_t>(remaining, sizeof(buffer));
    if (file.read(buffer, chunkSize) != static_cast<int>(chunkSize)) {
      LOG_ERR("ZIP", "Failed to read central directory for fingerprint");
      return false;
    }
    for (size_t i = 0; i < chunkSize; ++i) {
      hashByte(buffer[i]);
    }
    remaining -= chunkSize;
  }

  *fingerprint = hash;
  return true;
}

int ZipFile::fillUncompressedSizes(std::deque<SizeTarget>& targets, std::deque<uint32_t>& sizes) {
  if (targets.empty()) {
    return 0;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  if (!loadZipDetails()) return 0;

  file.seek(zipDetails.centralDirOffset);

  int matched = 0;
  const int targetCount = static_cast<int>(targets.size());
  uint32_t sig;
  char itemName[256];

  while (file.available()) {
    file.read(&sig, 4);
    if (sig != 0x02014b50) break;

    file.seekCur(6);
    uint16_t method;
    file.read(&method, 2);
    file.seekCur(8);
    uint32_t compressedSize, uncompressedSize;
    file.read(&compressedSize, 4);
    file.read(&uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    uint32_t localHeaderOffset;
    file.read(&localHeaderOffset, 4);

    if (nameLen < 256) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';

      uint64_t hash = fnvHash64(itemName, nameLen);
      SizeTarget key = {hash, nameLen, 0};

      auto it = std::lower_bound(targets.begin(), targets.end(), key, [](const SizeTarget& a, const SizeTarget& b) {
        return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
      });

      while (it != targets.end() && it->hash == hash && it->len == nameLen) {
        if (it->index < sizes.size()) {
          sizes[it->index] = uncompressedSize;
          matched++;
        }
        ++it;
      }

      if (matched >= targetCount) {
        break;
      }
    } else {
      file.seekCur(nameLen);
    }

    file.seekCur(m + k);
  }

  return matched;
}

uint8_t* ZipFile::readFileToMemory(const char* filename, size_t* size, const bool trailingNullByte) {
  const ScopedOpenClose zip{*this};
  if (!zip) return nullptr;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return nullptr;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return nullptr;

  file.seek(fileOffset);

  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;
  const auto dataSize = trailingNullByte ? inflatedDataSize + 1 : inflatedDataSize;
  const auto data = static_cast<uint8_t*>(malloc(dataSize));
  if (data == nullptr) {
    LOG_ERR("ZIP", "Failed to allocate memory for output buffer (%zu bytes)", dataSize);
    return nullptr;
  }

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const size_t dataRead = file.read(data, inflatedDataSize);

    if (dataRead != inflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data");
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else if (fileStat.method == ZIP_METHOD_DEFLATED) {
    // Read out deflated content from file
    const auto deflatedData = static_cast<uint8_t*>(malloc(deflatedDataSize));
    if (deflatedData == nullptr) {
      LOG_ERR("ZIP", "Failed to allocate memory for decompression buffer");
      free(data);
      return nullptr;
    }

    const size_t dataRead = file.read(deflatedData, deflatedDataSize);

    if (dataRead != deflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data, expected %d got %d", deflatedDataSize, dataRead);
      free(deflatedData);
      free(data);
      return nullptr;
    }

    bool success = false;
    {
      InflateReader r;
      r.init(false);
      r.setSource(deflatedData, deflatedDataSize);
      success = r.read(data, inflatedDataSize);
    }
    free(deflatedData);

    if (!success) {
      LOG_ERR("ZIP", "Failed to inflate file");
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else {
    LOG_ERR("ZIP", "Unsupported compression method");
    free(data);
    return nullptr;
  }

  if (trailingNullByte) data[inflatedDataSize] = '\0';
  if (size) *size = inflatedDataSize;
  return data;
}

bool ZipFile::readFileToStream(const char* filename, Print& out, const size_t chunkSize) {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return false;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return false;

  file.seek(fileOffset);
  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;
  LOG_DBG("ZIP", "Streaming %s (method=%s, chunk=%zu, compressed=%lu, uncompressed=%lu)", filename,
          zipMethodName(fileStat.method), chunkSize, static_cast<unsigned long>(deflatedDataSize),
          static_cast<unsigned long>(inflatedDataSize));

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    size_t allocatedChunkSize = 0;
    const auto buffer = allocateStreamChunk(chunkSize, allocatedChunkSize);
    if (!buffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for buffer (heap free=%u, maxAlloc=%u)", ESP.getFreeHeap(),
              ESP.getMaxAllocHeap());
      return false;
    }

    size_t remaining = inflatedDataSize;
    while (remaining > 0) {
      const size_t dataRead = file.read(buffer, remaining < allocatedChunkSize ? remaining : allocatedChunkSize);
      if (dataRead == 0) {
        LOG_ERR("ZIP", "Could not read more bytes");
        free(buffer);
        return false;
      }

      if (out.write(buffer, dataRead) != dataRead) {
        LOG_ERR("ZIP", "Failed to write all output bytes to stream (heap free=%u, maxAlloc=%u)", ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());
        free(buffer);
        return false;
      }
      remaining -= dataRead;
    }

    free(buffer);
    return true;
  }

  if (fileStat.method == ZIP_METHOD_DEFLATED) {
    if (inflatedDataSize == 0) {
      return true;
    }

    if (inflatedDataSize <= MAX_ONE_SHOT_STREAM_INFLATED_SIZE) {
      auto* deflatedData = static_cast<uint8_t*>(malloc(deflatedDataSize));
      if (!deflatedData) {
        LOG_ERR("ZIP", "Failed to allocate small deflated buffer (%lu bytes, heap free=%u, maxAlloc=%u)",
                static_cast<unsigned long>(deflatedDataSize), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        return false;
      }

      const size_t dataRead = file.read(deflatedData, deflatedDataSize);
      if (dataRead != deflatedDataSize) {
        LOG_ERR("ZIP", "Failed to read small deflated data, expected %lu got %zu",
                static_cast<unsigned long>(deflatedDataSize), dataRead);
        free(deflatedData);
        return false;
      }

      auto* inflatedData = static_cast<uint8_t*>(malloc(inflatedDataSize));
      if (!inflatedData) {
        LOG_ERR("ZIP", "Failed to allocate small inflated buffer (%lu bytes, heap free=%u, maxAlloc=%u)",
                static_cast<unsigned long>(inflatedDataSize), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        free(deflatedData);
        return false;
      }

      bool success = false;
      {
        InflateReader reader;
        success = reader.init(false);
        if (success) {
          reader.setSource(deflatedData, deflatedDataSize);
          success = reader.read(inflatedData, inflatedDataSize);
        }
      }
      free(deflatedData);

      if (!success) {
        LOG_ERR("ZIP", "Failed to inflate small deflated stream (%s)", filename);
        free(inflatedData);
        return false;
      }

      if (out.write(inflatedData, inflatedDataSize) != inflatedDataSize) {
        LOG_ERR("ZIP", "Failed to write small inflated stream (heap free=%u, maxAlloc=%u)", ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());
        free(inflatedData);
        return false;
      }

      LOG_DBG("ZIP", "One-shot streamed %s (%lu -> %lu bytes)", filename, static_cast<unsigned long>(deflatedDataSize),
              static_cast<unsigned long>(inflatedDataSize));
      free(inflatedData);
      return true;
    }

    size_t readChunkSize = 0;
    auto* fileReadBuffer = allocateStreamChunk(chunkSize, readChunkSize);
    if (!fileReadBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer (heap free=%u, maxAlloc=%u)",
              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      return false;
    }

    size_t outputChunkSize = 0;
    auto* outputBuffer = allocateStreamChunk(chunkSize, outputChunkSize);
    if (!outputBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for output buffer (heap free=%u, maxAlloc=%u)", ESP.getFreeHeap(),
              ESP.getMaxAllocHeap());
      free(fileReadBuffer);
      return false;
    }

    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = deflatedDataSize;
    ctx.readBuf = fileReadBuffer;
    ctx.readBufSize = readChunkSize;

    if (!ctx.reader.init(true)) {
      LOG_ERR("ZIP", "Failed to init inflate reader (heap free=%u, maxAlloc=%u)", ESP.getFreeHeap(),
              ESP.getMaxAllocHeap());
      free(outputBuffer);
      free(fileReadBuffer);
      return false;
    }
    ctx.reader.setReadCallback(zipReadCallback);

    bool success = false;
    size_t totalProduced = 0;

    while (true) {
      size_t produced;
      const InflateStatus status = ctx.reader.readAtMost(outputBuffer, outputChunkSize, &produced);

      totalProduced += produced;
      if (totalProduced > static_cast<size_t>(inflatedDataSize)) {
        LOG_ERR("ZIP", "Decompressed size exceeds expected (%zu > %zu)", totalProduced,
                static_cast<size_t>(inflatedDataSize));
        break;
      }

      if (produced > 0) {
        if (out.write(outputBuffer, produced) != produced) {
          LOG_ERR("ZIP", "Failed to write all output bytes to stream (heap free=%u, maxAlloc=%u)", ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());
          break;
        }
      }

      if (status == InflateStatus::Done) {
        if (totalProduced != static_cast<size_t>(inflatedDataSize)) {
          LOG_ERR("ZIP", "Decompressed size mismatch (expected %zu, got %zu)", static_cast<size_t>(inflatedDataSize),
                  totalProduced);
          break;
        }
        LOG_DBG("ZIP", "Decompressed %d bytes into %d bytes", deflatedDataSize, inflatedDataSize);
        success = true;
        break;
      }

      if (status == InflateStatus::Error) {
        LOG_ERR("ZIP", "Decompression failed");
        break;
      }
      // InflateStatus::Ok: output buffer full, continue
    }

    free(outputBuffer);
    free(fileReadBuffer);
    return success;  // ctx.reader destructor frees the ring buffer
  }

  LOG_ERR("ZIP", "Unsupported compression method %u for %s", fileStat.method, filename);
  return false;
}
