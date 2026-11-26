#include "TxtToHtml.h"

// Adapted from CrossPoint Reader #3704 (9c103c87f), under the MIT license.
#include <FsHelpers.h>
#include <Logging.h>
#include <memory>
#include <new>

#include <algorithm>
#include <cstring>

const char* TxtToHtml::cacheVersionTag(std::string_view filename) {
  return FsHelpers::hasMarkdownExtension(filename) ? "<!-- MD_CACHE_VERSION: yomuka-1 -->"
                                                 : "<!-- TXT_CACHE_VERSION: yomuka-1 -->";
}

bool TxtToHtml::stream(std::string_view filename, void* readerCtx, int (*readFn)(void* ctx, uint8_t* buf, size_t size),
                       Print& out) {
  constexpr size_t IN_BUF_SIZE = 1024;
  constexpr size_t OUT_BUF_SIZE = 2048;

  auto inBuf = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[IN_BUF_SIZE]);
  auto outBuf = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[OUT_BUF_SIZE]);
  if (!inBuf || !outBuf) {
    LOG_ERR("TXT", "OOM: TXT/MD HTML streaming buffers");
    return false;
  }

  size_t outPos = 0;
  bool outputOk = true;
  auto flushOut = [&]() {
    if (outPos > 0) {
      const size_t written = out.write(outBuf.get(), outPos);
      outputOk = outputOk && (written == outPos);
      outPos = 0;
    }
  };

  auto writeByte = [&](uint8_t b) {
    outBuf[outPos++] = b;
    if (outPos == OUT_BUF_SIZE) flushOut();
  };

  auto writeStr = [&](std::string_view s) {
    for (char c : s) {
      writeByte(static_cast<uint8_t>(c));
    }
  };

  writeStr("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
  writeStr(cacheVersionTag(filename));
  writeByte('\n');
  writeStr("<!DOCTYPE html>\n<html>\n<head><title>");
  const auto lastSlash = filename.find_last_of("/\\");
  std::string title(filename.substr(lastSlash == std::string_view::npos ? 0 : lastSlash + 1));
  const auto extension = title.find_last_of('.');
  if (extension != std::string::npos) title.resize(extension);
  for (char c : title) {
    if (c == '&')
      writeStr("&amp;");
    else if (c == '<')
      writeStr("&lt;");
    else if (c == '>')
      writeStr("&gt;");
    else
      writeByte(static_cast<uint8_t>(c));
  }
  writeStr("</title></head>\n<body>\n");

  bool isStart = true;
  bool atLineStart = true;
  size_t pendingSpaces = 0;
  // Readers may return short chunks, including inside the UTF-8 BOM.
  int bytesRead = 0;
  while (bytesRead < 3) {
    const int count = readFn(readerCtx, inBuf.get() + bytesRead, 3 - bytesRead);
    if (count < 0) return false;
    if (count == 0) break;
    bytesRead += count;
  }

  while (outputOk && bytesRead > 0) {
    int startIdx = 0;
    if (isStart) {
      isStart = false;
      if (bytesRead >= 3 && inBuf[0] == 0xEF && inBuf[1] == 0xBB && inBuf[2] == 0xBF) {
        startIdx = 3;
      }
    }

    for (int i = startIdx; i < bytesRead; i++) {
      uint8_t b = inBuf[i];
      if (b == '\r') continue;

      if (b == '\f') {
        pendingSpaces = 0;
        writeStr("<div style=\"page-break-after:always\"></div>");
        atLineStart = true;
        continue;
      }

      if (b == '\n') {
        pendingSpaces = 0;
        writeStr("<br />");
        atLineStart = true;
        continue;
      }

      if (b == ' ') {
        if (atLineStart) {
          writeStr("&#160;");
        } else {
          pendingSpaces++;
        }
        continue;
      }

      if (pendingSpaces > 0) {
        for (size_t s = 0; s < pendingSpaces - 1; s++) {
          writeStr("&#160;");
        }
        writeByte(' ');
        pendingSpaces = 0;
      }
      atLineStart = false;

      if (b == '&') {
        writeStr("&amp;");
      } else if (b == '<') {
        writeStr("&lt;");
      } else if (b == '>') {
        writeStr("&gt;");
      } else if (b < 0x20 && b != '\t') {
        writeByte(' ');
      } else {
        writeByte(b);
      }
    }
    if (outputOk) bytesRead = readFn(readerCtx, inBuf.get(), IN_BUF_SIZE);
  }

  if (bytesRead < 0) {
    LOG_ERR("TXT", "Read error while streaming TXT/MD: %.*s", static_cast<int>(filename.size()), filename.data());
    return false;
  }

  writeStr("\n</body>\n</html>\n");
  flushOut();
  if (!outputOk) {
    LOG_ERR("TXT", "Failed to stream complete HTML (write error or disk full)");
    return false;
  }
  return true;
}

bool TxtToHtml::stream(std::string_view filename, std::string_view content, Print& out) {
  struct ViewReader {
    std::string_view s;
    size_t pos = 0;
  } reader{content, 0};

  return stream(
      filename, &reader,
      [](void* ctx, uint8_t* buf, size_t size) -> int {
        auto* r = static_cast<ViewReader*>(ctx);
        if (r->pos >= r->s.size()) return 0;
        const size_t n = std::min(size, r->s.size() - r->pos);
        memcpy(buf, r->s.data() + r->pos, n);
        r->pos += n;
        return static_cast<int>(n);
      },
      out);
}
