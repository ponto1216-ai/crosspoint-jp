#include "Txt.h"

#include <FsHelpers.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Arduino.h>

#include <algorithm>
#include <cstring>

#include "Cp932Table.generated.h"
#include "TxtToHtml.h"

namespace {
bool isUtf8Continuation(const uint8_t value) { return (value & 0xC0) == 0x80; }

bool isValidUtf8(const uint8_t* data, const size_t length) {
  size_t i = 0;
  while (i < length) {
    const uint8_t first = data[i++];
    if (first < 0x80) continue;
    int continuationCount = 0;
    uint32_t codepoint = 0;
    if (first >= 0xC2 && first <= 0xDF) {
      continuationCount = 1;
      codepoint = first & 0x1F;
    } else if (first >= 0xE0 && first <= 0xEF) {
      continuationCount = 2;
      codepoint = first & 0x0F;
    } else if (first >= 0xF0 && first <= 0xF4) {
      continuationCount = 3;
      codepoint = first & 0x07;
    } else {
      return false;
    }
    // The detection sample can end in the middle of a valid UTF-8 character.
    if (i + continuationCount > length) return true;
    for (int j = 0; j < continuationCount; ++j) {
      if (!isUtf8Continuation(data[i])) return false;
      codepoint = (codepoint << 6) | (data[i++] & 0x3F);
    }
    if ((continuationCount == 2 && codepoint < 0x800) || (continuationCount == 3 && codepoint < 0x10000) ||
        codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
      return false;
    }
  }
  return true;
}

uint16_t cp932ToUnicode(const uint16_t value) {
  size_t left = 0;
  size_t right = kCp932TableSize;
  while (left < right) {
    const size_t mid = left + (right - left) / 2;
    if (kCp932Table[mid].sjis < value) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }
  return left < kCp932TableSize && kCp932Table[left].sjis == value ? kCp932Table[left].unicode : 0xFFFD;
}

void appendUtf8(std::string& output, const uint32_t codepoint) {
  if (codepoint <= 0x7F) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}
}  // namespace

Txt::Txt(std::string path, std::string cacheBasePath)
    : filepath(std::move(path)), cacheBasePath(std::move(cacheBasePath)) {
  // Generate cache path from file path hash
  const size_t hash = std::hash<std::string>{}(filepath);
  cachePath = this->cacheBasePath + "/txt_" + std::to_string(hash);
}

bool Txt::load() {
  if (loaded) {
    return true;
  }

  if (!Storage.exists(filepath.c_str())) {
    LOG_ERR("TXT", "File does not exist: %s", filepath.c_str());
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("TXT", filepath, file)) {
    LOG_ERR("TXT", "Failed to open file: %s", filepath.c_str());
    return false;
  }

  fileSize = file.size();
  uint8_t sample[4096] = {};
  const int sampleRead = file.read(sample, std::min(fileSize, sizeof(sample)));
  if (sampleRead < 0) {
    LOG_ERR("TXT", "Failed to read encoding sample: %s", filepath.c_str());
    return false;
  }
  const size_t sampleSize = static_cast<size_t>(sampleRead);
  if (sampleSize >= 3 && sample[0] == 0xEF && sample[1] == 0xBB && sample[2] == 0xBF) {
    encoding = Encoding::UTF8;
    contentOffset = 3;
  } else {
    encoding = isValidUtf8(sample, sampleSize) ? Encoding::UTF8 : Encoding::CP932;
    contentOffset = 0;
  }
  file.close();

  loaded = true;
  LOG_INF("TXT", "Loaded TXT file: %s (%zu bytes, encoding=%s, contentOffset=%zu)", filepath.c_str(), fileSize,
          encoding == Encoding::UTF8 ? "UTF-8" : "CP932", contentOffset);
  return true;
}

std::string Txt::getTitle() const {
  // Extract filename without path and extension
  size_t lastSlash = filepath.find_last_of('/');
  std::string filename = (lastSlash != std::string::npos) ? filepath.substr(lastSlash + 1) : filepath;

  // Remove .txt extension
  if (FsHelpers::hasTxtExtension(filename)) {
    filename = filename.substr(0, filename.length() - 4);
  }

  return filename;
}

void Txt::setupCacheDir() const {
  if (!Storage.exists(cacheBasePath.c_str())) {
    Storage.mkdir(cacheBasePath.c_str());
  }
  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  }
}

std::string Txt::findCoverImage() const {
  // Get the folder containing the txt file
  size_t lastSlash = filepath.find_last_of('/');
  std::string folder = (lastSlash != std::string::npos) ? filepath.substr(0, lastSlash) : "";
  if (folder.empty()) {
    folder = "/";
  }

  // Get the base filename without extension (e.g., "mybook" from "/books/mybook.txt")
  std::string baseName = getTitle();

  // Image extensions to try
  const char* extensions[] = {".bmp", ".jpg", ".jpeg", ".png", ".BMP", ".JPG", ".JPEG", ".PNG"};

  // First priority: look for image with same name as txt file (e.g., mybook.jpg)
  for (const auto& ext : extensions) {
    std::string coverPath = folder + "/" + baseName + ext;
    if (Storage.exists(coverPath.c_str())) {
      LOG_DBG("TXT", "Found matching cover image: %s", coverPath.c_str());
      return coverPath;
    }
  }

  // Fallback: look for cover image files
  const char* coverNames[] = {"cover", "Cover", "COVER"};
  for (const auto& name : coverNames) {
    for (const auto& ext : extensions) {
      std::string coverPath = folder + "/" + std::string(name) + ext;
      if (Storage.exists(coverPath.c_str())) {
        LOG_DBG("TXT", "Found fallback cover image: %s", coverPath.c_str());
        return coverPath;
      }
    }
  }

  return "";
}

std::string Txt::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

bool Txt::generateCoverBmp() const {
  // Already generated, return true
  if (Storage.exists(getCoverBmpPath().c_str())) {
    return true;
  }

  std::string coverImagePath = findCoverImage();
  if (coverImagePath.empty()) {
    LOG_DBG("TXT", "No cover image found for TXT file");
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  if (FsHelpers::hasBmpExtension(coverImagePath)) {
    // Copy BMP file to cache
    LOG_DBG("TXT", "Copying BMP cover image to cache");
    FsFile src, dst;
    if (!Storage.openFileForRead("TXT", coverImagePath, src)) {
      return false;
    }
    if (!Storage.openFileForWrite("TXT", getCoverBmpPath(), dst)) {
      return false;
    }
    uint8_t buffer[1024];
    while (src.available()) {
      size_t bytesRead = src.read(buffer, sizeof(buffer));
      dst.write(buffer, bytesRead);
    }
    LOG_DBG("TXT", "Copied BMP cover to cache");
    return true;
  } else if (FsHelpers::hasJpgExtension(coverImagePath)) {
    // Convert JPG/JPEG to BMP (same approach as Epub)
    LOG_DBG("TXT", "Generating BMP from JPG cover image");
    FsFile coverJpg, coverBmp;
    if (!Storage.openFileForRead("TXT", coverImagePath, coverJpg)) {
      return false;
    }
    if (!Storage.openFileForWrite("TXT", getCoverBmpPath(), coverBmp)) {
      return false;
    }
    const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp);

    if (!success) {
      LOG_ERR("TXT", "Failed to generate BMP from JPG cover image");
      Storage.remove(getCoverBmpPath().c_str());
    } else {
      LOG_DBG("TXT", "Generated BMP from JPG cover image");
    }
    return success;
  }

  // PNG files are not supported (would need a PNG decoder)
  LOG_ERR("TXT", "Cover image format not supported (only BMP/JPG/JPEG)");
  return false;
}

bool Txt::readContent(uint8_t* buffer, size_t offset, size_t length) const {
  if (!loaded) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("TXT", filepath, file)) {
    return false;
  }

  if (!file.seek(offset)) {
    return false;
  }

  size_t bytesRead = file.read(buffer, length);
  return bytesRead > 0;
}

bool Txt::readDecodedLine(const size_t offset, const size_t maxRawBytes, DecodedLine& out) const {
  FsFile file;
  if (!Storage.openFileForRead("TXT", filepath, file)) return false;
  return readDecodedLine(file, offset, maxRawBytes, out);
}

bool Txt::readDecodedLine(FsFile& file, const size_t offset, const size_t maxRawBytes, DecodedLine& out) const {
  out = {};
  out.nextOffset = offset;
  if (!loaded || offset >= fileSize || !file || (file.position() != offset && !file.seek(offset))) return false;

  size_t rawRead = 0;
  while (file.available() && rawRead < maxRawBytes) {
    const int next = file.read();
    if (next < 0) return false;
    const uint8_t first = static_cast<uint8_t>(next);
    ++rawRead;
    ++out.nextOffset;

    if (first == '\n') {
      out.newline = true;
      break;
    }
    if (first == '\f') {
      out.pageBreak = true;
      break;
    }
    if (first == '\r') continue;

    const size_t outputStart = out.text.size();
    if (encoding == Encoding::UTF8 || first < 0x80) {
      out.text.push_back(static_cast<char>(first));
      if (encoding == Encoding::UTF8 && first >= 0xC2) {
        int needed = first <= 0xDF ? 1 : (first <= 0xEF ? 2 : 3);
        // Once a multibyte character starts, finish it even if the raw chunk
        // boundary is reached so the next page never starts mid-character.
        while (needed-- > 0 && file.available()) {
          const int continuation = file.read();
          if (continuation < 0) return false;
          out.text.push_back(static_cast<char>(continuation));
          ++rawRead;
          ++out.nextOffset;
        }
      }
    } else if (first >= 0xA1 && first <= 0xDF) {
      appendUtf8(out.text, 0xFF61 + (first - 0xA1));
    } else if (((first >= 0x81 && first <= 0x9F) || (first >= 0xE0 && first <= 0xFC)) && file.available()) {
      const int trailValue = file.read();
      if (trailValue < 0) return false;
      ++rawRead;
      ++out.nextOffset;
      appendUtf8(out.text, cp932ToUnicode(static_cast<uint16_t>((first << 8) | trailValue)));
    } else {
      appendUtf8(out.text, 0xFFFD);
    }
    for (size_t i = outputStart; i < out.text.size(); ++i) out.rawEnds.push_back(out.nextOffset);
  }

  return out.nextOffset > offset;
}

bool Txt::streamToHtml(Print& output) {
  if (!load()) return false;
  FsFile input;
  if (!Storage.openFileForRead("TXT", filepath, input) || input.size() != fileSize) return false;

  struct Reader {
    Txt& txt;
    FsFile& input;
    size_t offset;
    DecodedLine line;
    size_t position = 0;
    uint8_t chunksRead = 0;

    int read(uint8_t* buffer, const size_t capacity) {
      if (position == line.text.size()) {
        if (offset >= txt.getFileSize()) return 0;
        if (!txt.readDecodedLine(input, offset, 1024, line)) return -1;
        if (++chunksRead == 64) {
          delay(1);
          chunksRead = 0;
        }
        offset = line.nextOffset;
        position = 0;
        if (line.newline) line.text.push_back('\n');
        if (line.pageBreak) line.text.push_back('\f');
        // A truncated read must never publish a successful XHTML cache.
        if (offset < txt.getFileSize() && !input.available()) return -1;
      }
      const size_t count = std::min(capacity, line.text.size() - position);
      if (count == 0) {
        // A chunk containing only CR still consumed source bytes.
        buffer[0] = '\r';
        return 1;
      }
      memcpy(buffer, line.text.data() + position, count);
      position += count;
      return static_cast<int>(count);
    }
  } reader{*this, input, contentOffset, {}};

  return TxtToHtml::stream(
      filepath, &reader,
      [](void* context, uint8_t* buffer, size_t capacity) {
        return static_cast<Reader*>(context)->read(buffer, capacity);
      },
      output);
}
