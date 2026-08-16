#include "ImageBlock.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Serialization.h>

#include "../converters/DirectPixelWriter.h"
#include "../converters/ImageCacheValidation.h"
#include "../converters/ImageDecoderFactory.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte), row-major order

ImageBlock::ImageBlock(const std::string& imagePath, int16_t width, int16_t height)
    : imagePath(imagePath), width(width), height(height) {}

bool ImageBlock::imageExists() const { return Storage.exists(imagePath.c_str()); }

namespace {

unsigned long failedJpegAt = 0;
constexpr unsigned long JPEG_RETRY_DELAY_MS = 30000;

std::string getCachePath(const std::string& imagePath) {
  // Version the cache whenever illustration tone/quantization changes.
  size_t dotPos = imagePath.rfind('.');
  if (dotPos != std::string::npos) {
    return imagePath.substr(0, dotPos) + ".pxc5";
  }
  return imagePath + ".pxc5";
}

// RAII guard: conditionally set skipDarkModeForImages so drawPixel skips
// dark-mode inversion for image pixels. When active=false the guard is a no-op.
struct ImageRenderScope {
  GfxRenderer& r;
  bool active;

  ImageRenderScope(GfxRenderer& r, bool active) : r(r), active(active) {
    if (active) r.beginImageRender();
  }

  ~ImageRenderScope() {
    if (active) r.endImageRender();
  }
};


bool renderFromCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                     int expectedHeight) {
  FsFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  ImageCacheValidation::PixelCacheInfo cacheInfo;
  if (!ImageCacheValidation::validatePixelCache(cacheFile, expectedWidth, expectedHeight, &cacheInfo)) {
    cacheFile.close();
    LOG_ERR("IMG", "Removing invalid pixel cache: %s", cachePath.c_str());
    Storage.remove(cachePath.c_str());
    return false;
  }
  const uint16_t cachedWidth = cacheInfo.width;
  const uint16_t cachedHeight = cacheInfo.height;

  // Use cached dimensions for rendering (they're the actual decoded size)
  expectedWidth = cachedWidth;
  expectedHeight = cachedHeight;

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  // Read several rows per SD access. A full-page image is re-rendered on every
  // grayscale strip pass (~14x per page), and a one-row-per-read loop here means
  // cachedHeight (~728) tiny reads through the storage mutex + SdFat each time —
  // the dominant cost of displaying an image page. Batching rows into a ~4KB
  // buffer cuts that to ~20 reads per pass without holding the whole image.
  const int bytesPerRow = (cachedWidth + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
  int rowsPerRead = 4096 / bytesPerRow;
  if (rowsPerRead < 1) rowsPerRead = 1;
  if (rowsPerRead > cachedHeight) rowsPerRead = cachedHeight;
  uint8_t* readBuffer = (uint8_t*)malloc((size_t)rowsPerRead * bytesPerRow);
  if (!readBuffer) {
    // Fall back to a single-row buffer under memory pressure.
    rowsPerRead = 1;
    readBuffer = (uint8_t*)malloc(bytesPerRow);
  }
  if (!readBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer");
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  int rowsInBuffer = 0;
  int bufferRow = 0;
  for (int row = 0; row < cachedHeight; row++) {
    if (bufferRow >= rowsInBuffer) {
      const int toRead = (cachedHeight - row < rowsPerRead) ? (cachedHeight - row) : rowsPerRead;
      const size_t bytes = (size_t)toRead * bytesPerRow;
      if (cacheFile.read(readBuffer, bytes) != static_cast<int>(bytes)) {
        LOG_ERR("IMG", "Cache read error at row %d", row);
        free(readBuffer);
        return false;
      }
      rowsInBuffer = toRead;
      bufferRow = 0;
    }

    const uint8_t* rowBuffer = readBuffer + (size_t)bufferRow * bytesPerRow;
    bufferRow++;

    const int destY = y + row;
    pw.beginRow(destY);
    // On a grayscale strip pass only a narrow column window of the image is in
    // the active band; skip the rest instead of unpacking+clipping every pixel.
    int colStart, colEnd;
    pw.bandColRange(x, cachedWidth, colStart, colEnd);
    for (int col = colStart; col < colEnd; col++) {
      const int byteIdx = col >> 2;            // col / 4
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      uint8_t pixelValue = (rowBuffer[byteIdx] >> bitShift) & 0x03;

      pw.writePixel(x + col, pixelValue);
    }
  }

  free(readBuffer);
  LOG_DBG("IMG", "Cache render complete");
  return true;
}

// Whether this image can be rendered from an existing BMP cache (JPEG only,
// in the pre-generated pixel-cache layout). A valid BMP cache means render()
// will not decode, so the font cache should be preserved across inline images.
bool hasValidBmpCache(const std::string& imagePath) {
  if (!FsHelpers::hasJpgExtension(imagePath)) return false;
  const std::string bmpPath = getCachePath(imagePath) + ".bmp";
  if (!Storage.exists(bmpPath.c_str())) return false;
  return ImageCacheValidation::validateBmpCacheFile(bmpPath);
}

}  // namespace

bool ImageBlock::pregeneratePngCache(GfxRenderer& renderer) const {
  if (!FsHelpers::hasPngExtension(imagePath)) return false;

  const std::string cachePath = getCachePath(imagePath);
  if (Storage.exists(cachePath.c_str())) {
    if (ImageCacheValidation::validatePixelCacheFile(cachePath, width, height)) return false;
    LOG_ERR("IMG", "Removing invalid PNG cache before pregeneration: %s", cachePath.c_str());
    Storage.remove(cachePath.c_str());
  }

  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
    fcm->freeKernLigatureData();
  }

  auto* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found while pregenerating: %s", imagePath.c_str());
    return false;
  }

  RenderConfig config;
  config.x = 0;
  config.y = 0;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.useExactDimensions = true;
  config.writeToFramebuffer = false;
  config.cachePath = cachePath;

  LOG_DBG("IMG", "Pregenerating PNG cache: %s", imagePath.c_str());
  return decoder->decodeToFramebuffer(imagePath, renderer, config) && Storage.exists(cachePath.c_str());
}

void ImageBlock::render(GfxRenderer& renderer, const int x, const int y) {
  // The font-prewarm scan pass only accumulates glyphs; an image contributes
  // none, and its DirectPixelWriter output bypasses the renderer's scan-mode
  // suppression, so it would otherwise do a full (discarded) cache render every
  // page view. Skip it here. The image still draws in the real BW/grayscale
  // passes; on first view this just moves the one-time decode to the BW pass.
  FontCacheManager* fcm = renderer.getFontCacheManager();
  if (fcm && fcm->isScanning()) return;

  LOG_DBG("IMG", "Rendering image at %d,%d: %s (%dx%d)", x, y, imagePath.c_str(), width, height);

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  // Bounds check render position using logical screen dimensions
  if (x < 0 || y < 0 || x + width > screenWidth || y + height > screenHeight) {
    LOG_ERR("IMG", "Invalid render position: (%d,%d) size (%dx%d) screen (%dx%d)", x, y, width, height, screenWidth,
            screenHeight);
    return;
  }

  // When "Invert Images" is OFF (default), skip dark mode inversion for images
  // and pre-fill with white so non-drawn pixels are visible on dark background.
  // When ON, let dark mode invert image pixels normally (no guard, no pre-fill).
  const bool skipInversion = renderer.isDarkMode() && !renderer.shouldInvertImagesInDarkMode();
  ImageRenderScope guard(renderer, skipInversion);
  if (skipInversion) {
    renderer.fillRect(x, y, width, height, false);
  }

  // Try to render from cache first
  std::string cachePath = getCachePath(imagePath);
  if (renderFromCache(renderer, cachePath, x, y, width, height)) {
    return;
  }

  // No cache - need to decode the image
  // Check if image file exists
  FsFile file;
  if (!Storage.openFileForRead("IMG", imagePath, file)) {
    LOG_ERR("IMG", "Image file not found: %s", imagePath.c_str());
    return;
  }
  size_t fileSize = file.size();
  file.close();

  if (fileSize == 0) {
    LOG_ERR("IMG", "Image file is empty: %s", imagePath.c_str());
    return;
  }

  // Page text prewarming can leave too little contiguous heap for the PNG
  // decoder on image-heavy chapters. These font caches are reproducible and
  // will reload on demand for any text after the image, while the generated
  // pixel cache lets the later grayscale passes avoid decoding altogether.
  //
  // BUT: for a JPEG that already has a valid BMP cache, no decode happens here
  // (we just read the cached BMP, a tiny heap footprint). Clearing the font
  // cache in that case only forces every glyph after the image to reload from
  // SD. On illustration pages with many small inline JPEGs the repeated
  // clearCache() caused the glyph reload storm behind the ~13s bw_render
  // (CrossPoint Reader). So skip the release when we can render from a cached
  // BMP; the caches are slot-limited and overflow-safe, so keeping them warm
  // across images is safe.
  if (fcm && !hasValidBmpCache(imagePath)) {
    fcm->clearCache();
    fcm->freeKernLigatureData();
    LOG_DBG("IMG", "Released font caches before decode: free=%u maxAlloc=%u", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
  }

  LOG_DBG("IMG", "Decoding and caching: %s", imagePath.c_str());

  RenderConfig config;
  config.x = x;
  config.y = y;
  config.maxWidth = width;
  config.maxHeight = height;
  config.useGrayscale = true;
  config.useDithering = true;
  config.performanceMode = false;
  config.useExactDimensions = true;  // Use pre-calculated dimensions to avoid rounding mismatches
  config.cachePath = cachePath;      // Enable caching during decode

  // For JPEG images, use the proven picojpeg-based converter (JpegToBmpConverter)
  // which correctly handles large images and scaling. The JPEGDEC-based
  // JpegToFramebufferConverter has diagonal distortion bugs with scaled output.
  // The converted BMP is cached on SD card for fast subsequent renders.
  if (FsHelpers::hasJpgExtension(imagePath)) {
    if (failedJpegAt != 0 &&
      static_cast<unsigned long>(millis() - failedJpegAt) < JPEG_RETRY_DELAY_MS) {
    LOG_DBG("IMG", "Skipping JPEG conversion during global cooldown: %s", imagePath.c_str());
    return;
  }
    const std::string bmpPath = cachePath + ".bmp";
    bool needsBmpCache = true;
    if (Storage.exists(bmpPath.c_str())) {
      if (ImageCacheValidation::validateBmpCacheFile(bmpPath)) {
        needsBmpCache = false;
      } else {
        LOG_DBG("IMG", "Removing invalid BMP cache: %s", bmpPath.c_str());
        Storage.remove(bmpPath.c_str());
      }
    }
    // Convert JPEG to BMP if not cached yet
    if (needsBmpCache) {
      FsFile jpegFile;
      if (!Storage.openFileForRead("IMG", imagePath, jpegFile)) {
        LOG_ERR("IMG", "Failed to open JPEG for BMP conversion: %s", imagePath.c_str());
        return;
      }

      FsFile bmpFile;
      if (!Storage.openFileForWrite("IMG", bmpPath, bmpFile)) {
        jpegFile.close();
        LOG_ERR("IMG", "Failed to create BMP cache file");
        return;
      }

      bool success = JpegToBmpConverter::jpegFileToBmpStreamWithSize(jpegFile, bmpFile, width, height);

      bmpFile.flush();
      const size_t bmpSizeBeforeClose = bmpFile.size();
      LOG_DBG("IMG", "Cached BMP size before close: %lu bytes", static_cast<unsigned long>(bmpSizeBeforeClose));

      jpegFile.close();
      bmpFile.close();

      if (!success || !ImageCacheValidation::validateBmpCacheFile(bmpPath)) {
        Storage.remove(bmpPath.c_str());

        failedJpegAt = millis();

        LOG_ERR("IMG", "JPEG to BMP conversion failed: %s", imagePath.c_str());
        return;
      }

      failedJpegAt = 0;

      LOG_DBG("IMG", "Cached JPEG as BMP: %s (%lu bytes)",
              bmpPath.c_str(),
              static_cast<unsigned long>(bmpSizeBeforeClose));

    }  // closes if (needsBmpCache)

    // Render from cached BMP
    FsFile bmpReadFile;
    if (Storage.openFileForRead("IMG", bmpPath, bmpReadFile)) {
      const size_t bmpReadSize = bmpReadFile.size();

      LOG_DBG("IMG", "Opening cached BMP: %s (%lu bytes)",
              bmpPath.c_str(),
              static_cast<unsigned long>(bmpReadSize));

      Bitmap bmp(bmpReadFile);
      const BmpReaderError bmpErr = bmp.parseHeaders();

      if (bmpErr == BmpReaderError::Ok) {
        LOG_DBG("IMG", "Cached BMP parsed: %dx%d, rowBytes=%u",
                bmp.getWidth(),
                bmp.getHeight(),
                static_cast<unsigned>(bmp.getRowBytes()));

        renderer.drawBitmap(bmp, x, y, width, height);
      } else {
        LOG_ERR("IMG", "Failed to parse cached BMP: %s",
                Bitmap::errorToString(bmpErr));
        Storage.remove(bmpPath.c_str());
      }

      bmpReadFile.close();
    } else {
      LOG_ERR("IMG", "Failed to open cached BMP: %s", bmpPath.c_str());
    }

    return;
  }

  // For non-JPEG images (PNG), use the direct framebuffer decoder
  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(imagePath);
  if (!decoder) {
    LOG_ERR("IMG", "No decoder found for image: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Using %s decoder", decoder->getFormatName());

  bool success = decoder->decodeToFramebuffer(imagePath, renderer, config);
  if (!success) {
    LOG_ERR("IMG", "Failed to decode image: %s", imagePath.c_str());
    return;
  }

  LOG_DBG("IMG", "Decode successful");
}

bool ImageBlock::serialize(FsFile& file) {
  serialization::writeString(file, imagePath);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  return true;
}

std::unique_ptr<ImageBlock> ImageBlock::deserialize(FsFile& file) {
  std::string path;
  serialization::readString(file, path);
  int16_t w, h;
  serialization::readPod(file, w);
  serialization::readPod(file, h);
  return std::unique_ptr<ImageBlock>(new ImageBlock(path, w, h));
}
