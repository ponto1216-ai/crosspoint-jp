#include "HttpDownloader.h"

#include <HTTPClient.h>
#include <Logging.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <StreamString.h>
#include <WiFi.h>
#include <base64.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

#include "CrossPointSettings.h"
#include "util/UrlUtils.h"

int HttpDownloader::lastHttpCode = 0;

namespace {
void logHttpFailure(const char* operation, const std::string& url, int errorCode) {
  const String localIp = WiFi.localIP().toString();
  const String errorText = HTTPClient::errorToString(errorCode);
  LOG_ERR("HTTP", "%s failed: code=%d (%s), wifi=%d rssi=%d ip=%s heap=%u maxAlloc=%u url=%s", operation, errorCode,
          errorText.c_str(), static_cast<int>(WiFi.status()), WiFi.RSSI(), localIp.c_str(), ESP.getFreeHeap(),
          ESP.getMaxAllocHeap(), url.c_str());
}

class FileWriteStream final : public Stream {
 public:
  FileWriteStream(FsFile& file, size_t total, size_t initialDownloaded, HttpDownloader::ProgressCallback progress,
                  HttpDownloader::CancelCallback shouldCancel)
      : file_(file),
        total_(total),
        downloaded_(initialDownloaded),
        progress_(std::move(progress)),
        shouldCancel_(std::move(shouldCancel)) {}

  size_t write(uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    // Write-through stream for HTTPClient::writeToStream with progress tracking.
    const size_t written = file_.write(buffer, size);
    if (written != size) {
      writeOk_ = false;
    }
    downloaded_ += written;
    if (progress_ && total_ > 0) {
      progress_(downloaded_, total_);
    }
    return written;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override { file_.flush(); }

  size_t downloaded() const { return downloaded_; }
  bool ok() const { return writeOk_; }
  bool shouldAbort() {
    if (shouldCancel_ && shouldCancel_()) aborted_ = true;
    return aborted_;
  }
  bool aborted() const { return aborted_; }

 private:
  FsFile& file_;
  size_t total_;
  size_t downloaded_ = 0;
  bool writeOk_ = true;
  bool aborted_ = false;
  HttpDownloader::ProgressCallback progress_;
  HttpDownloader::CancelCallback shouldCancel_;
};

constexpr size_t HTTP_STREAM_BUFFER_SIZE = 512;
constexpr unsigned long HTTP_STREAM_IDLE_TIMEOUT_MS = 30000;

bool copyResponseBytes(NetworkClient& client, FileWriteStream& output, size_t remaining,
                       unsigned long streamIdleTimeoutMs) {
  uint8_t buffer[HTTP_STREAM_BUFFER_SIZE];
  unsigned long lastDataAt = millis();
  while (remaining > 0) {
    if (output.shouldAbort()) return false;

    const size_t available = client.available();
    if (available == 0) {
      // A TLS socket may report disconnected while it still has unread bytes
      // buffered locally. Check available() first; only fail once both the
      // buffer is empty and the connection has closed.
      if (!client.connected()) {
        LOG_ERR("HTTP", "Response stream closed (remaining=%zu)", remaining);
        return false;
      }
      if (millis() - lastDataAt >= streamIdleTimeoutMs) {
        LOG_ERR("HTTP", "Response stream timed out after %lums (remaining=%zu)", streamIdleTimeoutMs, remaining);
        return false;
      }
      delay(1);
      continue;
    }

    size_t requested = available < sizeof(buffer) ? available : sizeof(buffer);
    if (requested > remaining) requested = remaining;
    const size_t read = client.readBytes(buffer, requested);
    if (read == 0) {
      LOG_ERR("HTTP", "Response stream read failed (requested=%zu remaining=%zu)", requested, remaining);
      return false;
    }
    if (output.write(buffer, read) != read) {
      LOG_ERR("HTTP", "Response stream write failed (read=%zu remaining=%zu)", read, remaining);
      return false;
    }
    remaining -= read;
    lastDataAt = millis();
  }
  return true;
}

bool readChunkSize(NetworkClient& client, size_t& chunkSize) {
  char line[24] = {};
  size_t length = 0;
  while (length < sizeof(line) - 1) {
    uint8_t ch = 0;
    if (client.readBytes(&ch, 1) != 1) return false;
    if (ch == '\n') break;
    if (ch != '\r') line[length++] = static_cast<char>(ch);
  }
  if (length == sizeof(line) - 1) return false;

  char* end = nullptr;
  const unsigned long parsed = strtoul(line, &end, 16);
  if (end == line) return false;
  chunkSize = static_cast<size_t>(parsed);
  return true;
}

bool streamHttpResponse(HTTPClient& http, FileWriteStream& output, unsigned long streamIdleTimeoutMs) {
  NetworkClient* client = http.getStreamPtr();
  if (!client) return false;

  const int64_t contentLength = http.getSize();
  if (contentLength >= 0) {
    return copyResponseBytes(*client, output, static_cast<size_t>(contentLength), streamIdleTimeoutMs);
  }

  // HTTPClient normally allocates a 4 KB buffer to decode chunks. Keep the
  // conversion response streamable on the ESP32-C3's constrained heap instead.
  while (true) {
    size_t chunkSize = 0;
    if (!readChunkSize(*client, chunkSize)) return false;
    if (chunkSize == 0) return true;
    if (!copyResponseBytes(*client, output, chunkSize, streamIdleTimeoutMs)) return false;

    uint8_t trailing[2] = {};
    if (client->readBytes(trailing, sizeof(trailing)) != sizeof(trailing) || trailing[0] != '\r' ||
        trailing[1] != '\n') {
      return false;
    }
  }
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  // Use NetworkClientSecure for HTTPS, regular NetworkClient for HTTP
  std::unique_ptr<NetworkClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new NetworkClientSecure();
    secureClient->setInsecure();
    secureClient->setHandshakeTimeout(20);
    client.reset(secureClient);
  } else {
    client.reset(new NetworkClient());
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Fetching: %s", url.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  if (!username.empty() && !password.empty()) {
    std::string credentials = username + ":" + password;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  LOG_DBG("HTTP", "Free heap before GET: %d", ESP.getFreeHeap());
  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  LOG_DBG("HTTP", "GET result: %d, free heap: %d", httpCode, ESP.getFreeHeap());
  if (httpCode != HTTP_CODE_OK) {
    logHttpFailure("Fetch", url, httpCode);
    http.end();
    return false;
  }

  const int writeResult = http.writeToStream(&outContent);
  http.end();

  if (writeResult < 0) {
    LOG_ERR("HTTP", "writeToStream failed: %d", writeResult);
    lastHttpCode = writeResult;
    return false;
  }

  LOG_DBG("HTTP", "Fetch success: %d bytes", writeResult);
  return true;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  // Direct string fetch: avoids StreamString and writeToStream issues.
  std::unique_ptr<NetworkClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new NetworkClientSecure();
    secureClient->setInsecure();
    secureClient->setHandshakeTimeout(20);
    client.reset(secureClient);
  } else {
    client.reset(new NetworkClient());
  }
  HTTPClient http;

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  if (!username.empty() && !password.empty()) {
    std::string credentials = username + ":" + password;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  LOG_DBG("HTTP", "FetchStr: %s (heap=%d)", url.c_str(), ESP.getFreeHeap());
  const int httpCode = http.GET();
  lastHttpCode = httpCode;

  if (httpCode != HTTP_CODE_OK) {
    logHttpFailure("FetchStr", url, httpCode);
    http.end();
    return false;
  }

  // Read body in small chunks to avoid large single allocation.
  // TLS buffers (~40KB) are held during the connection, leaving limited heap.
  NetworkClient* stream = http.getStreamPtr();
  const int contentLen = http.getSize();
  outContent.clear();
  if (contentLen > 0) {
    outContent.reserve(contentLen);
  }

  char buf[512];
  while (stream->available() || stream->connected()) {
    int avail = stream->available();
    if (avail <= 0) {
      delay(1);
      continue;
    }
    int toRead = (avail < static_cast<int>(sizeof(buf))) ? avail : static_cast<int>(sizeof(buf));
    int bytesRead = stream->readBytes(buf, toRead);
    if (bytesRead > 0) {
      outContent.append(buf, bytesRead);
    } else {
      break;
    }
  }
  http.end();

  if (outContent.empty()) {
    LOG_ERR("HTTP", "FetchStr: empty body (contentLen=%d)", contentLen);
    lastHttpCode = -901;
    return false;
  }

  LOG_DBG("HTTP", "FetchStr success: %zu bytes", outContent.size());
  return true;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, int timeoutMs,
                                                             const std::string& username, const std::string& password,
                                                             size_t resumeFrom, CancelCallback shouldCancel,
                                                             bool preservePartialOnError,
                                                             unsigned long streamIdleTimeoutMs) {
  // Use NetworkClientSecure for HTTPS, regular NetworkClient for HTTP
  std::unique_ptr<NetworkClient> client;
  if (UrlUtils::isHttpsUrl(url)) {
    auto* secureClient = new NetworkClientSecure();
    secureClient->setInsecure();
    secureClient->setHandshakeTimeout(20);
    client.reset(secureClient);
  } else {
    client.reset(new NetworkClient());
  }
  HTTPClient http;

  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  http.begin(*client, url.c_str());
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (timeoutMs > 0) {
    http.setTimeout(timeoutMs);
  }
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  const bool resumeRequested = resumeFrom > 0 && Storage.exists(destPath.c_str());
  if (resumeRequested) {
    http.addHeader("Range", "bytes=" + String(resumeFrom) + "-");
  } else {
    resumeFrom = 0;
  }

  if (!username.empty() && !password.empty()) {
    std::string credentials = username + ":" + password;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  lastHttpCode = httpCode;
  const bool resuming = resumeRequested && httpCode == HTTP_CODE_PARTIAL_CONTENT;
  if (httpCode != HTTP_CODE_OK && !resuming) {
    logHttpFailure("Download", url, httpCode);
    http.end();
    return HTTP_ERROR;
  }

  if (resumeRequested && !resuming) {
    LOG_DBG("HTTP", "Range request ignored; restarting download from zero");
    resumeFrom = 0;
  }

  const int64_t reportedLength = http.getSize();
  const size_t contentLength = reportedLength > 0 ? static_cast<size_t>(reportedLength) : 0;
  if (contentLength > 0) {
    LOG_DBG("HTTP", "Content-Length: %zu", contentLength);
  } else {
    LOG_DBG("HTTP", "Content-Length: unknown");
  }

  // A server that does not support Range replies with 200. Start over in that
  // case rather than appending a complete response to a partial file.
  if (!resuming && Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }

  // Append only after a successful 206 response. Otherwise write a clean file.
  FsFile file;
  const bool fileOpened = resuming ? static_cast<bool>(file = Storage.open(destPath.c_str(), O_WRONLY | O_APPEND))
                                   : Storage.openFileForWrite("HTTP", destPath.c_str(), file);
  if (!fileOpened) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    http.end();
    return FILE_ERROR;
  }

  // Stream in small pieces to avoid HTTPClient's temporary 4 KB receive buffer.
  const size_t totalLength = contentLength > 0 ? resumeFrom + contentLength : 0;
  FileWriteStream fileStream(file, totalLength, resumeFrom, std::move(progress), std::move(shouldCancel));
  const unsigned long idleTimeout = streamIdleTimeoutMs > 0 ? streamIdleTimeoutMs : HTTP_STREAM_IDLE_TIMEOUT_MS;
  const bool streamOk = streamHttpResponse(http, fileStream, idleTimeout);

  file.close();
  http.end();

  if (fileStream.aborted()) {
    LOG_INF("HTTP", "Download cancelled (written=%zu)", fileStream.downloaded());
    return ABORTED;
  }

  if (!streamOk) {
    LOG_ERR("HTTP", "Response stream failed (len=%zu, written=%zu)", contentLength, fileStream.downloaded());
    lastHttpCode = -902;  // Custom code: response stream read/write failure.
    if (!preservePartialOnError || !fileStream.ok()) {
      Storage.remove(destPath.c_str());
    } else {
      LOG_INF("HTTP", "Keeping partial download for resume: %s (%zu bytes)", destPath.c_str(),
              fileStream.downloaded());
    }
    return HTTP_ERROR;
  }

  const size_t downloaded = fileStream.downloaded();
  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);

  // Guard against partial writes even if HTTPClient completes.
  if (!fileStream.ok()) {
    LOG_ERR("HTTP", "Write failed during download");
    lastHttpCode = -900;  // Custom code: SD write failure
    Storage.remove(destPath.c_str());
    return FILE_ERROR;
  }

  if (contentLength == 0 && downloaded == 0) {
    LOG_ERR("HTTP", "Download failed: no data received");
    lastHttpCode = -901;  // Custom code: no data
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  // Verify download size if known
  if (totalLength > 0 && downloaded != totalLength) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu", downloaded, totalLength);
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  return OK;
}
