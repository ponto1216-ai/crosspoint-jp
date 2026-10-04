#!/usr/bin/env python3
"""Exercise actual upload handlers/input polling with modeled multipart events.

Storage/connection spies cover cleanup and last-chunk END-before-ABORT ordering.
SnapshotUpload is real; durable snapshot storage is tested by the exchange suite.
This does not execute the TCP parser or emulate physical input hardware.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default=os.environ.get('CXX', 'c++'))
parser.add_argument('--zig', action='store_true')
args = parser.parse_args()


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


web = (root / 'src/network/CrossPointWebServer.cpp').read_text(encoding='utf-8')
activity = (root / 'src/activities/network/CrossPointWebServerActivity.cpp').read_text(encoding='utf-8')
methods = '\n'.join(function(web, signature) for signature in [
    'bool CrossPointWebServer::dropUploadIfCancelled()',
    'static bool flushUploadBuffer(',
    'void CrossPointWebServer::handleUpload(',
    'void CrossPointWebServer::handleSyncUpload()',
    'void CrossPointWebServer::handleSyncUploadPost()',
])
poll = function(activity, 'bool CrossPointWebServerActivity::pollUploadCancel()')
source = r'''
#include <ArduinoJson.h>
#include "sync/SnapshotUpload.h"
#include <cassert>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
template<class... Args> void testLog(Args&&...) {}
#define LOG_DBG(...) testLog(__VA_ARGS__)
#define LOG_ERR(...) testLog(__VA_ARGS__)
#define LOG_INF(...) testLog(__VA_ARGS__)
struct String : std::string {
  using std::string::string;
  String() = default;
  String(const std::string& s) : std::string(s) {}
  bool isEmpty() const { return empty(); }
  bool endsWith(const char* s) const {
    auto n = std::strlen(s); return size() >= n && compare(size()-n, n, s) == 0;
  }
};
std::map<std::string, std::string> files;
int writes = 0, cacheClears = 0, publishes = 0;
std::string staged = "previous committed snapshot";
struct FsFile {
  bool opened = false;
  std::string path;
  explicit operator bool() const { return opened; }
  void close() { opened = false; }
  size_t write(const uint8_t* bytes, size_t n) {
    assert(opened); ++writes; files[path].append(reinterpret_cast<const char*>(bytes), n); return n;
  }
};
struct {
  bool exists(const char* p) { return files.count(p); }
  bool remove(const char* p) { return files.erase(p); }
  bool mkdir(const char*) { return true; }
  bool openFileForWrite(const char*, const String& p, FsFile& f) {
    f.opened = true; f.path = p; files[p] = ""; return true;
  }
} Storage;
namespace FsHelpers {
bool isSafePathComponent(const String& s) { return !s.empty() && s.find('/') == s.npos; }
bool hasBmpExtension(const std::string& s) { return s.size() >= 4 && s.substr(s.size()-4) == ".bmp"; }
}
String normalizeWebPath(const String& s) { return s; }
void clearEpubCacheIfNeeded(const String&) { ++cacheClears; }
namespace Issue18Diagnostics { void logMemory(const char*, const char*) {} }
unsigned long now = 0, uploadStartTime = 0, totalWriteTime = 0;
size_t writeCount = 0;
unsigned long millis() { return now; }
void delay(unsigned ms) { now += ms; }
void resetTaskWatchdogIfSubscribed() {}
constexpr int MALLOC_CAP_8BIT = 1;
size_t heap_caps_get_free_size(int) { return 1000000; }
size_t heap_caps_get_largest_free_block(int) { return 1000000; }
struct { size_t getFreeHeap() { return 1000000; } size_t getMaxAllocHeap() { return 1000000; } } ESP;
constexpr char WEB_SLEEP_IMAGE_DIR[] = "/.sleep", WEB_SLEEP_OVERLAY_DIR[] = "/.sleep-overlay";
enum { UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED };
struct HTTPUpload {
  int status = UPLOAD_FILE_START;
  String filename = "cancel-test.txt";
  size_t currentSize = 0;
  uint8_t buf[5000]{};
};
struct Client {
  bool alive = true;
  int stops = 0;
  bool connected() const { return alive; }
  void stop() { alive = false; ++stops; }
};
struct Server {
  HTTPUpload part;
  Client connection;
  int responses = 0;
  HTTPUpload& upload() { return part; }
  Client& client() { return connection; }
  bool hasArg(const char*) { return false; }
  String arg(const char*) { return ""; }
  void send(int, const char*, const char*) { ++responses; }
};
namespace yomuka::sync {
struct ExchangeBook {};
enum class ExchangeError { None, Invalid, Storage };
SnapshotError parseSnapshot(const uint8_t* bytes, size_t length, JsonDocument& output) {
  return deserializeJson(output, bytes, length) ? SnapshotError::Json : SnapshotError::None;
}
ExchangeError stageSnapshotForBook(const ExchangeBook&, const JsonDocument&) {
  ++publishes; staged = "new committed snapshot"; return ExchangeError::None;
}
}
struct CrossPointWebServer {
  struct UploadState {
    FsFile file;
    String fileName, path = "/", error, errorCode;
    size_t size = 0, bufferPos = 0;
    bool success = false, structuredResponse = false;
    static constexpr size_t UPLOAD_BUFFER_SIZE = 4096;
    std::vector<uint8_t> buffer = std::vector<uint8_t>(UPLOAD_BUFFER_SIZE);
  } upload;
  std::unique_ptr<Server> server = std::make_unique<Server>();
  bool running = true;
  std::function<bool()> uploadCancelCheck;
  yomuka::sync::SnapshotUpload syncUpload;
  size_t syncDiagnosticNeeded = 0, syncDiagnosticFree = 0, syncDiagnosticLargest = 0;
  bool getSyncBook(yomuka::sync::ExchangeBook&) const { return true; }
  bool dropUploadIfCancelled() const;
  void handleUpload(UploadState&) const;
  void handleSyncUpload();
  void handleSyncUploadPost();
};
struct MappedInputManager { enum class Button { Back }; };
struct Input {
  int updates = 0;
  int pressAt = -1;
  bool pressed = false, edge = false;
  void update() { edge = ++updates == pressAt; pressed = edge; }
  bool isPressed(MappedInputManager::Button) { return pressed; }
  bool wasPressed(MappedInputManager::Button) { return edge; }
};
struct { bool raw = false; bool rawInputActive() { return raw; } } gpio;
struct CrossPointWebServerActivity {
  Input mappedInput;
  bool leaveRequested = false;
  bool pollUploadCancel();
};
''' + methods + '\n' + poll + r'''
void event(CrossPointWebServer& web, int status, size_t n, bool snapshot = false) {
  web.server->part.status = status; web.server->part.currentSize = n;
  if (snapshot) web.handleSyncUpload(); else web.handleUpload(web.upload);
}
int main() {
  // Debounce second sample, first-sample edge retention and latched cancellation.
  for (int at : {1, 2}) {
    CrossPointWebServerActivity activity;
    gpio.raw = true; activity.mappedInput.pressAt = at;
    unsigned before = now;
    assert(activity.pollUploadCancel());
    assert(activity.mappedInput.updates == at);
    assert(now - before == (at == 2 ? 6u : 0u));
    int updates = activity.mappedInput.updates;
    assert(activity.pollUploadCancel() && activity.mappedInput.updates == updates);
  }
  gpio.raw = false;
  { CrossPointWebServerActivity a; assert(!a.pollUploadCancel() && a.mappedInput.updates == 1); }
  for (bool lastChunk : {false, true}) {
    CrossPointWebServer web;
    event(web, UPLOAD_FILE_START, 0);
    event(web, UPLOAD_FILE_WRITE, 4500);
    assert(writes > 0 && web.upload.file && web.upload.bufferPos > 0);
    int beforeWrites = writes, beforeClears = cacheClears;
    web.uploadCancelCheck = [] { return true; };
    event(web, UPLOAD_FILE_WRITE, 100);
    assert(web.server->connection.stops == 1 && writes == beforeWrites);
    // The installed parser emits END after the final WRITE, even if WRITE stopped the client.
    if (lastChunk) event(web, UPLOAD_FILE_END, 0);
    assert(!web.upload.success && writes == beforeWrites && cacheClears == beforeClears);
    event(web, UPLOAD_FILE_ABORTED, 0);
    assert(!web.upload.file && web.upload.bufferPos == 0 && web.upload.error == "Upload aborted");
    assert(!files.count("/cancel-test.txt"));
  }
  // Normal transfer and restart/retry still flush and finish.
  { CrossPointWebServer web;
    event(web, UPLOAD_FILE_START, 0); event(web, UPLOAD_FILE_WRITE, 123);
    event(web, UPLOAD_FILE_END, 0);
    assert(web.upload.success && !web.upload.file && files.at("/cancel-test.txt").size() == 123);
  }
  const std::string payload = R"({"snapshot":"valid-for-handler-spy"})";
  for (bool lastChunk : {false, true}) {
    CrossPointWebServer web;
    event(web, UPLOAD_FILE_START, 0, true);
    std::memcpy(web.server->part.buf, payload.data(), payload.size());
    event(web, UPLOAD_FILE_WRITE, payload.size(), true);
    assert(web.syncUpload.size() == payload.size());
    std::string previous = staged;
    int beforePublish = publishes;
    web.uploadCancelCheck = [] { return true; };
    event(web, UPLOAD_FILE_WRITE, 1, true);
    assert(web.syncUpload.size() == 0);
    if (lastChunk) event(web, UPLOAD_FILE_END, 0, true);
    web.handleSyncUploadPost();  // Defense even if an unexpected post callback follows disconnection.
    event(web, UPLOAD_FILE_ABORTED, 0, true);
    assert(staged == previous && publishes == beforePublish && web.server->responses == 0);
    // A new request after network abort, without activity exit, can be received normally.
    web.server->connection.alive = true; web.uploadCancelCheck = {};
    event(web, UPLOAD_FILE_START, 0, true);
    event(web, UPLOAD_FILE_WRITE, payload.size(), true);
    event(web, UPLOAD_FILE_END, 0, true); web.handleSyncUploadPost();
    assert(publishes == beforePublish + 1 && web.server->responses == 1);
  }
  // Remote disconnect at END also cannot publish or finalize.
  { CrossPointWebServer web; event(web, UPLOAD_FILE_START, 0);
    web.server->connection.alive = false; event(web, UPLOAD_FILE_END, 0);
    assert(!web.upload.success); event(web, UPLOAD_FILE_ABORTED, 0);
  }
  puts("PASS: input latch/debounce, file cleanup, last-chunk ordering, snapshot retention and retry");
}
'''
with tempfile.TemporaryDirectory(prefix='yomuka-http-cancel-') as directory:
    path = Path(directory)
    cpp = path / 'test.cpp'
    cpp.write_text(source, encoding='utf-8')
    exe = path / 'test.exe'
    command = [args.compiler] + (['c++'] if args.zig else [])
    subprocess.run(command + ['-std=c++20', '-Wall', '-Wextra', '-Werror',
                             '-I' + str(root / 'src'), '-isystem',
                             str(root / '.pio/libdeps/default/ArduinoJson/src'),
                             str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
