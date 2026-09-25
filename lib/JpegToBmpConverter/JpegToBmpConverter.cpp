#include "JpegToBmpConverter.h"

#include <BuildScratch.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <JPEGDEC.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <new>

#include "BitmapHelpers.h"

// ============================================================================
// IMAGE PROCESSING OPTIONS - Toggle these to test different configurations
// ============================================================================
constexpr bool USE_8BIT_OUTPUT = false;  // true: 8-bit grayscale (no quantization), false: 2-bit (4 levels)
// Dithering method selection (only one should be true, or all false for simple quantization):
constexpr bool USE_ATKINSON = false;         // Atkinson dithering (cleaner than F-S, less error diffusion)
constexpr bool USE_FLOYD_STEINBERG = false;  // Floyd-Steinberg error diffusion (can cause "worm" artifacts)
constexpr bool USE_NOISE_DITHERING = false;  // Hash-based noise dithering (good for downsampling)
// Pre-resize to target display size (CRITICAL: avoids dithering artifacts from post-downsampling)
constexpr bool USE_PRESCALE = true;  // true: scale image to target size before dithering
// ============================================================================

inline void write16(Print& out, const uint16_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
}

inline void write32(Print& out, const uint32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

inline void write32Signed(Print& out, const int32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

// Helper function: Write BMP header with 8-bit grayscale (256 levels)
void writeBmpHeader8bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 3) / 4 * 4;  // 8 bits per pixel, padded
  const int imageSize = bytesPerRow * height;
  const uint32_t paletteSize = 256 * 4;  // 256 colors * 4 bytes (BGRA)
  const uint32_t fileSize = 14 + 40 + paletteSize + imageSize;

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);                      // Reserved
  write32(bmpOut, 14 + 40 + paletteSize);  // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 8);              // Bits per pixel (8 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 256);   // colorsUsed
  write32(bmpOut, 256);   // colorsImportant

  // Color Palette (256 grayscale entries x 4 bytes = 1024 bytes)
  for (int i = 0; i < 256; i++) {
    bmpOut.write(static_cast<uint8_t>(i));  // Blue
    bmpOut.write(static_cast<uint8_t>(i));  // Green
    bmpOut.write(static_cast<uint8_t>(i));  // Red
    bmpOut.write(static_cast<uint8_t>(0));  // Reserved
  }
}

// Helper function: Write BMP header with 1-bit color depth (black and white)
static void writeBmpHeader1bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 31) / 32 * 4;  // 1 bit per pixel, round up to 4-byte boundary
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 62 + imageSize;  // 14 (file header) + 40 (DIB header) + 8 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 62);        // Offset to pixel data (14 + 40 + 8)

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 1);              // Bits per pixel (1 bit)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 2);     // colorsUsed
  write32(bmpOut, 2);     // colorsImportant

  // Color Palette (2 colors x 4 bytes = 8 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  // Note: In 1-bit BMP, palette index 0 = black, 1 = white
  uint8_t palette[8] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0xFF, 0xFF, 0xFF, 0x00   // Color 1: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

// Helper function: Write BMP header with 2-bit color depth
static void writeBmpHeader2bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;  // 2 bits per pixel, round up
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;  // 14 (file header) + 40 (DIB header) + 16 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 70);        // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 2);              // Bits per pixel (2 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 4);     // colorsUsed
  write32(bmpOut, 4);     // colorsImportant

  // Color Palette (4 colors x 4 bytes = 16 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  uint8_t palette[16] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0x55, 0x55, 0x55, 0x00,  // Color 1: Dark gray (85)
      0xAA, 0xAA, 0xAA, 0x00,  // Color 2: Light gray (170)
      0xFF, 0xFF, 0xFF, 0x00   // Color 3: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

namespace {

// Max MCU height supported by any JPEG (4:2:0 chroma = 16 rows, 4:4:4 = 8 rows)
constexpr int MAX_MCU_HEIGHT = 16;
constexpr size_t JPEG_DECODER_SIZE = 20 * 1024;
constexpr size_t MIN_FREE_HEAP = JPEG_DECODER_SIZE + 8 * 1024;
constexpr size_t BMP_WRITE_BUFFER_SIZE = 4096;

// JPEG conversion emits one small BMP row at a time.  Writing every row
// directly to SD turns a normal screen-sized illustration into hundreds of
// individual filesystem writes.  Buffer the byte-identical BMP stream so the
// cache generator and the on-demand renderer use larger SD writes instead.
class TimedBufferedPrint final : public Print {
 public:
  explicit TimedBufferedPrint(Print& output)
      : output(output), buffer(new (std::nothrow) uint8_t[BMP_WRITE_BUFFER_SIZE]) {}

  ~TimedBufferedPrint() override { delete[] buffer; }

  bool isBuffered() const { return buffer != nullptr; }
  uint32_t getWriteMs() const { return writeMs; }
  size_t getBytesWritten() const { return bytesWritten; }

  size_t write(uint8_t value) override { return write(&value, 1); }

  size_t write(const uint8_t* data, size_t size) override {
    if (failed) return 0;
    if (!buffer) {
      const uint32_t startedAt = millis();
      const size_t written = output.write(data, size);
      writeMs += millis() - startedAt;
      if (written != size) {
        LOG_ERR("JPG", "Short direct BMP write: %u/%u", static_cast<unsigned>(written), static_cast<unsigned>(size));
        failed = true;
        return 0;
      }
      bytesWritten += written;
      return written;
    }

    size_t accepted = 0;
    while (size > 0) {
      const size_t available = BMP_WRITE_BUFFER_SIZE - used;
      const size_t chunk = size < available ? size : available;
      memcpy(buffer + used, data, chunk);
      used += chunk;
      data += chunk;
      size -= chunk;
      accepted += chunk;
      if (used == BMP_WRITE_BUFFER_SIZE && !flushOutput()) return 0;
    }
    return accepted;
  }

  bool flushOutput() {
    if (failed || used == 0) return !failed;
    const uint32_t startedAt = millis();
    const size_t written = output.write(buffer, used);
    writeMs += millis() - startedAt;
    if (written != used) {
      LOG_ERR("JPG", "Short buffered BMP write: %u/%u", static_cast<unsigned>(written), static_cast<unsigned>(used));
      failed = true;
      return false;
    }
    bytesWritten += written;
    used = 0;
    return true;
  }

 private:
  Print& output;
  uint8_t* buffer = nullptr;
  size_t used = 0;
  size_t bytesWritten = 0;
  uint32_t writeMs = 0;
  bool failed = false;
};

// Static file pointer for JPEGDEC open callback.
// Safe in single-threaded embedded context; never accessed concurrently.
static FsFile* s_jpegFile = nullptr;

void* bmpJpegOpen(const char* /*filename*/, int32_t* size) {
  if (!s_jpegFile || !*s_jpegFile) return nullptr;
  s_jpegFile->seek(0);
  *size = static_cast<int32_t>(s_jpegFile->size());
  return s_jpegFile;
}

void bmpJpegClose(void* /*handle*/) {
  // Caller owns the file — do not close it here
}

int32_t bmpJpegRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
  auto* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return 0;
  int32_t n = f->read(pBuf, len);
  if (n < 0) n = 0;
  pFile->iPos += n;
  return n;
}

int32_t bmpJpegSeek(JPEGFILE* pFile, int32_t pos) {
  auto* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f || !f->seek(pos)) return -1;
  pFile->iPos = pos;
  return pos;
}

// Context passed to the JPEGDEC draw callback via setUserPointer()
struct BmpConvertCtx {
  Print* bmpOut;
  int srcWidth;
  int srcHeight;
  int outWidth;
  int outHeight;
  bool oneBit;
  bool useIllustrationRendering;
  int bytesPerRow;
  bool needsScaling;
  int callbackCount;
  int rowsWritten;
  uint32_t scaleX_fp;  // source pixels per output pixel, 16.16 fixed-point
  uint32_t scaleY_fp;

  // Accumulates one MCU row (up to MAX_MCU_HEIGHT source rows × srcWidth pixels)
  // Filled column-by-column as JPEGDEC callbacks arrive for the same MCU row
  uint8_t* mcuBuf;
  uint8_t* mcuScratch;

  // Y-axis area averaging accumulators (needsScaling only)
  int currentOutY;
  uint32_t nextOutY_srcStart;  // 16.16 fixed-point boundary for the next output row
  uint32_t* rowAccum;
  uint32_t* rowCount;

  uint8_t* bmpRow;

  AtkinsonDitherer* atkinsonDitherer;
  FloydSteinbergDitherer* fsDitherer;
  Atkinson1BitDitherer* atkinson1BitDitherer;

  bool error;
};
static uint8_t darkenForEpaperImage(const uint8_t gray) {
  int v = gray;

  // Lower value = darker. 0=black, 255=white.
  v = (v - 128) * 140 / 100 + 128;  // contrast +40%
  v -= 50;                          // darken image

  if (v < 0) v = 0;
  if (v > 255) v = 255;
  return static_cast<uint8_t>(v);
}
// Write a fully-assembled output row (grayscale bytes, length outWidth) to BMP
static void writeOutputRow(BmpConvertCtx* ctx, const uint8_t* srcRow, int outY) {
  memset(ctx->bmpRow, 0, ctx->bytesPerRow);

  if (USE_8BIT_OUTPUT && !ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      ctx->bmpRow[x] = adjustPixel(srcRow[x]);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(srcRow[x], x)
                                                    : quantize1bit(srcRow[x], x, outY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = ctx->useIllustrationRendering ? applyIllustrationToneCurve(adjustPixel(srcRow[x]))
                                                         : darkenForEpaperImage(adjustPixel(srcRow[x]));
      uint8_t twoBit;
      if (ctx->useIllustrationRendering) {
        twoBit = applyBayerDither4Level(gray, x, outY);
      } else if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, outY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  const size_t written = ctx->bmpOut->write(ctx->bmpRow, ctx->bytesPerRow);
  if (written != static_cast<size_t>(ctx->bytesPerRow)) {
    LOG_ERR("JPG", "Short BMP row write: %u/%d", static_cast<unsigned>(written), ctx->bytesPerRow);
    ctx->error = true;
    return;
  }
  ctx->rowsWritten++;
}

// Flush one scaled output row from Y-axis accumulators and advance currentOutY
static void flushScaledRow(BmpConvertCtx* ctx) {
  memset(ctx->bmpRow, 0, ctx->bytesPerRow);

  if (USE_8BIT_OUTPUT && !ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      ctx->bmpRow[x] = adjustPixel(gray);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(gray, x)
                                                    : quantize1bit(gray, x, ctx->currentOutY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t sourceGray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      const uint8_t gray = ctx->useIllustrationRendering ? applyIllustrationToneCurve(adjustPixel(sourceGray))
                                                         : darkenForEpaperImage(adjustPixel(sourceGray));
      uint8_t twoBit;
      if (ctx->useIllustrationRendering) {
        twoBit = applyBayerDither4Level(gray, x, ctx->currentOutY);
      } else if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, ctx->currentOutY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  const size_t written = ctx->bmpOut->write(ctx->bmpRow, ctx->bytesPerRow);
  if (written != static_cast<size_t>(ctx->bytesPerRow)) {
    LOG_ERR("JPG", "Short scaled BMP row write: %u/%d", static_cast<unsigned>(written), ctx->bytesPerRow);
    ctx->error = true;
    return;
  }
  ctx->rowsWritten++;
  ctx->currentOutY++;
}

// JPEGDEC draw callback — receives one MCU-width × MCU-height block at a time,
// in left-to-right, top-to-bottom order (baseline JPEG).
// Accumulates columns into mcuBuf; once the last column arrives (completing the MCU
// row), applies scaling + dithering and writes packed BMP rows to bmpOut.
int bmpDrawCallback(JPEGDRAW* pDraw) {
  auto* ctx = reinterpret_cast<BmpConvertCtx*>(pDraw->pUser);
  if (!ctx || ctx->error) return 0;

  ctx->callbackCount++;

  if (ctx->callbackCount <= 8) {
    LOG_DBG("JPG", "Callback #%d: x=%d y=%d w=%d used=%d h=%d", ctx->callbackCount, pDraw->x, pDraw->y, pDraw->iWidth,
            pDraw->iWidthUsed, pDraw->iHeight);
  }

  const uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;

  // Some JPEGDEC builds leave iWidthUsed as 0 even when iWidth is valid.
  // If validW stays 0, the callback never reaches the "last MCU column" path,
  // so only the BMP header is written.
  const int validW = (pDraw->iWidthUsed > 0) ? pDraw->iWidthUsed : pDraw->iWidth;

  const int blockH = pDraw->iHeight;
  const int blockX = pDraw->x;
  const int blockY = pDraw->y;
  if (blockY == 0) {
    LOG_DBG("JPG", "Draw block x=%d y=%d w=%d used=%d h=%d src=%dx%d", blockX, blockY, pDraw->iWidth, pDraw->iWidthUsed,
            blockH, ctx->srcWidth, ctx->srcHeight);
  }
  // Copy block pixels into MCU row buffer
  for (int r = 0; r < blockH && r < MAX_MCU_HEIGHT; r++) {
    const int copyW = (blockX + validW <= ctx->srcWidth) ? validW : (ctx->srcWidth - blockX);
    if (copyW <= 0) continue;
    memcpy(ctx->mcuBuf + r * ctx->srcWidth + blockX, pixels + r * stride, copyW);
  }

  // Wait for the last MCU column before processing any rows
  if (blockX + validW < ctx->srcWidth) return 1;

  // Process each complete source row in this MCU row
  const int endRow = blockY + blockH;

  for (int y = blockY; y < endRow && y < ctx->srcHeight; y++) {
    const uint8_t* srcRow = ctx->mcuBuf + (y - blockY) * ctx->srcWidth;

    if (!ctx->needsScaling) {
      // 1:1 — outWidth == srcWidth, write directly
      writeOutputRow(ctx, srcRow, y);
    } else {
      // Fixed-point area averaging on X axis
      for (int outX = 0; outX < ctx->outWidth; outX++) {
        const int srcXStart = (static_cast<uint32_t>(outX) * ctx->scaleX_fp) >> 16;
        const int srcXEnd = (static_cast<uint32_t>(outX + 1) * ctx->scaleX_fp) >> 16;
        int sum = 0;
        int count = 0;
        for (int srcX = srcXStart; srcX < srcXEnd && srcX < ctx->srcWidth; srcX++) {
          sum += srcRow[srcX];
          count++;
        }
        if (count == 0 && srcXStart < ctx->srcWidth) {
          sum = srcRow[srcXStart];
          count = 1;
        }
        ctx->rowAccum[outX] += sum;
        ctx->rowCount[outX] += count;
      }

      // Flush output row(s) whose Y boundary we've crossed
      const uint32_t srcY_fp = static_cast<uint32_t>(y + 1) << 16;
      while (srcY_fp >= ctx->nextOutY_srcStart && ctx->currentOutY < ctx->outHeight) {
        flushScaledRow(ctx);
        ctx->nextOutY_srcStart = static_cast<uint32_t>(ctx->currentOutY + 1) * ctx->scaleY_fp;
        if (srcY_fp >= ctx->nextOutY_srcStart) continue;
        memset(ctx->rowAccum, 0, ctx->outWidth * sizeof(uint32_t));
        memset(ctx->rowCount, 0, ctx->outWidth * sizeof(uint32_t));
      }
    }
  }

  return ctx->error ? 0 : 1;
}

}  // namespace

// Internal implementation with configurable target size and bit depth
bool JpegToBmpConverter::jpegFileToBmpStreamInternal(FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                                     bool oneBit, bool crop, bool useIllustrationRendering) {
  const uint32_t conversionStartedAt = millis();
  LOG_DBG("JPG", "Converting JPEG to %s BMP (target: %dx%d)", oneBit ? "1-bit" : "2-bit", targetWidth, targetHeight);
  if (useIllustrationRendering) LOG_INF("IMGQ", "JPEG illustration: full-res + PNG Bayer tone v5");

  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    LOG_ERR("JPG", "Not enough heap for JPEG decoder (%u free, need %u)", ESP.getFreeHeap(), MIN_FREE_HEAP);
    return false;
  }

  TimedBufferedPrint bufferedOutput(bmpOut);
  if (!bufferedOutput.isBuffered()) {
    LOG_DBG("JPG", "BMP write buffer unavailable; using direct writes");
  }

  s_jpegFile = &jpegFile;

  JPEGDEC* jpeg = new (std::nothrow) JPEGDEC();
  if (!jpeg) {
    LOG_ERR("JPG", "Failed to allocate JPEG decoder");
    return false;
  }

  int rc = jpeg->open("", bmpJpegOpen, bmpJpegClose, bmpJpegRead, bmpJpegSeek, bmpDrawCallback);
  if (rc != 1) {
    LOG_ERR("JPG", "JPEG open failed (err=%d)", jpeg->getLastError());
    return false;
  }

  const int srcWidth = jpeg->getWidth();
  const int srcHeight = jpeg->getHeight();

  LOG_DBG("JPG", "JPEG dimensions: %dx%d", srcWidth, srcHeight);
  const int decodeScale = useIllustrationRendering ? 1 : 2;
  const int decWidth = (srcWidth + decodeScale - 1) / decodeScale;
  const int decHeight = (srcHeight + decodeScale - 1) / decodeScale;

  LOG_DBG("JPG", "JPEG decoded dimensions: %dx%d", decWidth, decHeight);

  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;

  if (srcWidth <= 0 || srcHeight <= 0 || srcWidth > MAX_IMAGE_WIDTH || srcHeight > MAX_IMAGE_HEIGHT) {
    LOG_DBG("JPG", "Image too large or invalid (%dx%d), max supported: %dx%d", srcWidth, srcHeight, MAX_IMAGE_WIDTH,
            MAX_IMAGE_HEIGHT);
    jpeg->close();
    return false;
  }

  // Calculate output dimensions (pre-scale to fit display exactly)
  int outWidth = decWidth;
  int outHeight = decHeight;
  uint32_t scaleX_fp = 65536;  // 1.0 in 16.16 fixed point
  uint32_t scaleY_fp = 65536;
  bool needsScaling = false;

  if (targetWidth > 0 && targetHeight > 0 && (srcWidth != targetWidth || srcHeight != targetHeight)) {
    const float scaleToFitWidth = static_cast<float>(targetWidth) / srcWidth;
    const float scaleToFitHeight = static_cast<float>(targetHeight) / srcHeight;
    float scale = 1.0f;
    if (crop) {
      scale = (scaleToFitWidth > scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    } else {
      scale = (scaleToFitWidth < scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    }

    outWidth = static_cast<int>(srcWidth * scale);
    outHeight = static_cast<int>(srcHeight * scale);
    if (outWidth < 1) outWidth = 1;
    if (outHeight < 1) outHeight = 1;

    scaleX_fp = (static_cast<uint32_t>(decWidth) << 16) / outWidth;
    scaleY_fp = (static_cast<uint32_t>(decHeight) << 16) / outHeight;
    needsScaling = decWidth != outWidth || decHeight != outHeight;

    LOG_DBG("JPG", "Scaling %dx%d -> %dx%d (target %dx%d)", srcWidth, srcHeight, outWidth, outHeight, targetWidth,
            targetHeight);
  }

  // Write BMP header with output dimensions
  int bytesPerRow;
  if (USE_8BIT_OUTPUT && !oneBit) {
    writeBmpHeader8bit(bufferedOutput, outWidth, outHeight);
    bytesPerRow = (outWidth + 3) / 4 * 4;
  } else if (oneBit) {
    writeBmpHeader1bit(bufferedOutput, outWidth, outHeight);
    bytesPerRow = (outWidth + 31) / 32 * 4;
  } else {
    writeBmpHeader2bit(bufferedOutput, outWidth, outHeight);
    bytesPerRow = (outWidth * 2 + 31) / 32 * 4;
  }

  BmpConvertCtx ctx = {};
  ctx.bmpOut = &bufferedOutput;
  ctx.srcWidth = decWidth;
  ctx.srcHeight = decHeight;
  ctx.outWidth = outWidth;
  ctx.outHeight = outHeight;
  ctx.oneBit = oneBit;
  ctx.useIllustrationRendering = useIllustrationRendering;
  ctx.bytesPerRow = bytesPerRow;
  ctx.needsScaling = needsScaling;
  ctx.scaleX_fp = scaleX_fp;
  ctx.scaleY_fp = scaleY_fp;
  ctx.error = false;

  // RAII guard: frees all heap resources on any return path
  struct Cleanup {
    BmpConvertCtx& ctx;
    JPEGDEC* jpeg;

    ~Cleanup() {
      delete[] ctx.rowAccum;
      delete[] ctx.rowCount;
      delete ctx.atkinson1BitDitherer;
      if (ctx.mcuScratch)
        buildscratch::release(ctx.mcuScratch);
      else
        free(ctx.mcuBuf);
      free(ctx.bmpRow);

      if (jpeg) {
        jpeg->close();
        delete jpeg;
      }

      s_jpegFile = nullptr;
    }
  } cleanup{ctx, jpeg};

  // MCU row buffer: MAX_MCU_HEIGHT rows × srcWidth columns of grayscale
  const size_t mcuBytes = static_cast<size_t>(MAX_MCU_HEIGHT) * ctx.srcWidth;
  ctx.mcuScratch = buildscratch::claim(mcuBytes);
  ctx.mcuBuf = ctx.mcuScratch ? ctx.mcuScratch : static_cast<uint8_t*>(malloc(mcuBytes));
  if (!ctx.mcuBuf) {
    LOG_ERR("JPG", "Failed to allocate MCU buffer (%u bytes, free=%u, maxAlloc=%u)", static_cast<unsigned>(mcuBytes),
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return false;
  }
  memset(ctx.mcuBuf, 0, mcuBytes);

  ctx.bmpRow = static_cast<uint8_t*>(malloc(bytesPerRow));
  if (!ctx.bmpRow) {
    LOG_ERR("JPG", "Failed to allocate BMP row buffer");
    return false;
  }

  if (needsScaling) {
    ctx.rowAccum = new (std::nothrow) uint32_t[outWidth]();
    ctx.rowCount = new (std::nothrow) uint32_t[outWidth]();
    if (!ctx.rowAccum || !ctx.rowCount) {
      LOG_ERR("JPG", "Failed to allocate scaling buffers");
      return false;
    }
    ctx.nextOutY_srcStart = scaleY_fp;
  }

  if (oneBit) {
    ctx.atkinson1BitDitherer = new (std::nothrow) Atkinson1BitDitherer(outWidth);
  } else if (!USE_8BIT_OUTPUT) {
    if (USE_ATKINSON) {
      ctx.atkinsonDitherer = new (std::nothrow) AtkinsonDitherer(outWidth);
    } else if (USE_FLOYD_STEINBERG) {
      ctx.fsDitherer = new (std::nothrow) FloydSteinbergDitherer(outWidth);
    }
  }

  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&ctx);

  rc = jpeg->decode(0, 0, useIllustrationRendering ? 0 : JPEG_SCALE_HALF);

  const bool flushSuccess = bufferedOutput.flushOutput();

  LOG_DBG("JPG", "JPEG decode result: rc=%d callbacks=%d rows=%d expectedRows=%d err=%d", rc, ctx.callbackCount,
          ctx.rowsWritten, ctx.outHeight, jpeg->getLastError());

  if (rc != 1 || ctx.error || !flushSuccess || ctx.rowsWritten != ctx.outHeight) {
    LOG_ERR("JPG", "JPEG decode incomplete: rc=%d callbacks=%d rows=%d/%d err=%d", rc, ctx.callbackCount,
            ctx.rowsWritten, ctx.outHeight, jpeg->getLastError());
    return false;
  }

  const uint32_t totalMs = millis() - conversionStartedAt;
  const uint32_t writeMs = bufferedOutput.getWriteMs();
  LOG_DBG("JPG", "JPEG timing: total=%lu ms, decode/resize/dither=%lu ms, BMP write=%lu ms, output=%lu bytes", totalMs,
          totalMs >= writeMs ? totalMs - writeMs : 0, writeMs,
          static_cast<unsigned long>(bufferedOutput.getBytesWritten()));
  LOG_DBG("JPG", "Successfully converted JPEG to BMP");
  return true;
}

// Core function: Convert JPEG file to 2-bit BMP (uses default target size)
bool JpegToBmpConverter::jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, bool crop) {
  // Use runtime display dimensions (swapped for portrait cover sizing)
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetWidth, targetHeight, false, crop, false);
}

// Convert with custom target size (for thumbnails, 2-bit)
bool JpegToBmpConverter::jpegFileToBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                     int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, false, false, true);
}

// Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                         int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, true, true, false);
}
