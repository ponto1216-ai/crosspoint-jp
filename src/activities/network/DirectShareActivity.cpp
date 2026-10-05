#include "DirectShareActivity.h"

#include <ESPmDNS.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>
#include <cstdio>

#include "WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "components/UiLayout.h"
#include "fontIds.h"
#include "sync/BatchSnapshot.h"
#include "sync/SnapshotUpload.h"
#include "sync/StorageIo.h"

using namespace yomuka::sync;
namespace {
constexpr unsigned long IdleTimeout = 10000, RequestTimeout = 30000;
size_t freeMemory() { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }
size_t largestMemory() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }
}  // namespace
void DirectShareActivity::onEnter() {
  Activity::onEnter();
  state = receiving ? State::Wifi : State::Prepare;
  if (receiving) connectWifi();
  requestUpdate();
}
void DirectShareActivity::onExit() {
  socket.stop();
  listener.end();
  MDNS.end();
  incoming.clear();
  body.reset();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Activity::onExit();
}
void DirectShareActivity::connectWifi() {
  state = State::Wifi;
  if (WiFi.status() == WL_CONNECTED) {
    connected();
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled || !std::get<WifiResult>(result.data).connected) {
                             finish();
                             return;
                           }
                           connected();
                         });
}
void DirectShareActivity::connected() {
  const auto mac = WiFi.macAddress();
  std::string suffix = mac.c_str();
  suffix.erase(std::remove(suffix.begin(), suffix.end(), ':'), suffix.end());
  suffix = suffix.substr(suffix.size() > 6 ? suffix.size() - 6 : 0);
  device = std::string(gpio.deviceIsX3() ? "X3-" : "X4-") + suffix;
  const std::string host = "yomuka-" + suffix;
  MDNS.end();
  const bool mdns = MDNS.begin(host.c_str());
  if (!mdns) {
    fail(tr(STR_DIRECT_DISCOVERY_ERROR));
    return;
  }
  if (receiving) {
    char session[17];
    snprintf(session, sizeof(session), "%08lx%08lx", static_cast<unsigned long>(esp_random()),
             static_cast<unsigned long>(esp_random()));
    token = session;
    listener.begin();
    if (!listener) {
      fail(tr(STR_DIRECT_NETWORK_ERROR));
      return;
    }
    MDNS.addService("yomuka-share", "tcp", 80);
    MDNS.addServiceTxt("yomuka-share", "tcp", "version", "1");
    state = State::Receive;
  } else
    state = State::Discover;
  requestUpdate();
}
bool DirectShareActivity::cancelled() {
  const bool seen = mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                    mappedInput.isPressed(MappedInputManager::Button::Back);
  mappedInput.update();
  const bool cancel = seen || mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                      mappedInput.isPressed(MappedInputManager::Button::Back);
  if (cancel) consumeBackRelease = true;
  return cancel;
}
void DirectShareActivity::fail(const char* text) {
  socket.stop();
  incoming.clear();
  header.clear();
  reply.clear();
  headerDone = false;
  state = State::Message;
  message = text;
  requestUpdate();
}
void DirectShareActivity::prepare() {
  BatchSnapshot batch;
  std::vector<ShareCandidate> source{book};
  batch.start({1});
  batch.step(source, READING_HISTORY, [this] { return cancelled(); });
  if (batch.results()[0].result == BatchResult::Cancelled) {
    fail(tr(STR_DIRECT_CANCELLED));
    return;
  }
  if (batch.results()[0].result != BatchResult::Saved) {
    fail(tr(STR_DIRECT_PREPARE_ERROR));
    return;
  }
  const auto path = exchangeFilePath(book.bookId);
  HalFile file;
  if (!Storage.openFileForRead("DIRECT", path, file)) {
    fail(tr(STR_DIRECT_PREPARE_ERROR));
    return;
  }
  bodySize = file.size();
  if (!file.close() || !bodySize || !SnapshotUpload::memoryAvailable(bodySize, freeMemory(), largestMemory())) {
    fail(tr(STR_DIRECT_MEMORY_ERROR));
    return;
  }
  body.reset(new (std::nothrow) uint8_t[bodySize]);
  size_t count = 0;
  if (!body || readBytes(path, body.get(), bodySize, count) != ReadStatus::Present || count != bodySize) {
    fail(tr(STR_DIRECT_PREPARE_ERROR));
    return;
  }
  char id[17];
  snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(book.bookId));
  identity = id;
  {
    JsonDocument snapshot;
    if (parseSnapshot(body.get(), bodySize, snapshot) != SnapshotError::None || snapshot["bookId"] != identity) {
      fail(tr(STR_DIRECT_PREPARE_ERROR));
      return;
    }
  }
  digest = directDigest(body.get(), bodySize);
  connectWifi();
}
void DirectShareActivity::discover() {
  peers.clear();
  cursor = 0;
  const int count = MDNS.queryService("yomuka-share", "tcp");
  for (int i = 0; i < count && peers.size() < 8; ++i) {
    const auto ip = MDNS.address(i);
    if (ip == WiFi.localIP() || ip == IPAddress(0, 0, 0, 0) || !MDNS.port(i)) continue;
    peers.push_back({MDNS.hostname(i).c_str(), ip, MDNS.port(i)});
  }
  if (cancelled()) {
    fail(tr(STR_DIRECT_CANCELLED));
    return;
  }
  state = State::Peers;
  requestUpdate();
}
void DirectShareActivity::beginRequest(bool upload) {
  socket.stop();
  header.clear();
  reply.clear();
  parsed = {};
  headerDone = false;
  sent = 0;
  if (cursor >= peers.size()) {
    fail(tr(STR_DIRECT_NETWORK_ERROR));
    return;
  }
  const auto& peer = peers[cursor];
  socket.setConnectionTimeout(1000);
  socket.setTimeout(1000);
  if (!socket.connect(peer.ip, peer.port, 1000)) {
    fail(tr(STR_DIRECT_NETWORK_ERROR));
    return;
  }
  std::string request = upload ? "POST /yomuka/snapshot HTTP/1.1\r\n" : "GET /yomuka/hello HTTP/1.1\r\n";
  request += "Host: " + std::string(peer.ip.toString().c_str()) + "\r\nConnection: close\r\n";
  if (upload)
    request += "Content-Type: application/json\r\nContent-Length: " + std::to_string(bodySize) +
               "\r\nX-Yomuka-Session: " + token + "\r\n";
  request += "\r\n";
  if (socket.write(reinterpret_cast<const uint8_t*>(request.data()), request.size()) != request.size()) {
    fail(tr(STR_DIRECT_NETWORK_ERROR));
    return;
  }
  startedIo = lastIo = millis();
  posted = upload;
  state = upload ? State::Send : State::Hello;
  requestUpdate();
}
void DirectShareActivity::readReply() {
  const auto now = millis();
  if (now - lastIo > IdleTimeout || now - startedIo > RequestTimeout) {
    fail(posted ? tr(STR_DIRECT_UNCERTAIN) : tr(STR_DIRECT_NETWORK_ERROR));
    return;
  }
  unsigned budget = 1024;
  while (socket.available() && budget--) {
    const int value = socket.read();
    if (value < 0) break;
    lastIo = now;
    if (!headerDone) {
      header += static_cast<char>(value);
      if (header.size() > 2048) {
        fail(posted ? tr(STR_DIRECT_UNCERTAIN) : tr(STR_DIRECT_NETWORK_ERROR));
        return;
      }
      if (header.size() >= 4 && header.substr(header.size() - 4) == "\r\n\r\n") {
        if (!parseDirectHeader(header, true, parsed)) {
          fail(posted ? tr(STR_DIRECT_UNCERTAIN) : tr(STR_DIRECT_NETWORK_ERROR));
          return;
        }
        headerDone = true;
      }
    } else {
      reply += static_cast<char>(value);
      if (reply.size() == parsed.length) {
        JsonDocument doc;
        if (parsed.status != 200 || deserializeJson(doc, reply) != DeserializationError::Ok) {
          fail(posted && parsed.status == 200 ? tr(STR_DIRECT_UNCERTAIN) : tr(STR_DIRECT_REJECTED));
          return;
        }
        socket.stop();
        if (posted) {
          const bool accepted = directAccepted(doc, identity, digest, bodySize);
          if (accepted) cancelled();
          fail(accepted ? tr(STR_DIRECT_SENT) : tr(STR_DIRECT_UNCERTAIN));
        } else {
          std::string label;
          if (!directHello(doc, label, token)) {
            fail(tr(STR_DIRECT_REJECTED));
            return;
          }
          peers[cursor].name = label;
          state = State::Confirm;
          requestUpdate();
        }
        return;
      }
    }
  }
  if (!socket.connected() && !socket.available())
    fail(posted ? tr(STR_DIRECT_UNCERTAIN) : tr(STR_DIRECT_NETWORK_ERROR));
}
void DirectShareActivity::respond(int status, const JsonDocument& doc) {
  std::string text;
  serializeJson(doc, text);
  const auto response = "HTTP/1.1 " + std::to_string(status) +
                        " Result\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(text.size()) +
                        "\r\nConnection: close\r\n\r\n" + text;
  socket.write(reinterpret_cast<const uint8_t*>(response.data()), response.size());
  socket.stop();
  header.clear();
  parsed = {};
  headerDone = false;
  incoming.clear();
}
void DirectShareActivity::receive() {
  if (!socket && listener.hasClient()) {
    socket = listener.accept();
    socket.setTimeout(1000);
    startedIo = lastIo = millis();
    header.clear();
    parsed = {};
    headerDone = false;
    incoming.clear();
  }
  if (!socket && !socket.available()) {
    incoming.clear();
    header.clear();
    headerDone = false;
    return;
  }
  const auto now = millis();
  if (now - lastIo > IdleTimeout || now - startedIo > RequestTimeout) {
    socket.stop();
    incoming.clear();
    header.clear();
    headerDone = false;
    return;
  }
  unsigned budget = 1024;
  while (socket.available() && budget--) {
    const int value = socket.read();
    if (value < 0) break;
    lastIo = now;
    if (!headerDone) {
      header += static_cast<char>(value);
      if (header.size() > 2048) {
        socket.stop();
        header.clear();
        return;
      }
      if (header.size() >= 4 && header.substr(header.size() - 4) == "\r\n\r\n") {
        if (!parseDirectHeader(header, false, parsed)) {
          JsonDocument error;
          error["error"] = "request";
          respond(400, error);
          return;
        }
        if (parsed.method == "GET") {
          JsonDocument hello;
          hello["protocol"] = "yomuka-direct";
          hello["version"] = 1;
          hello["device"] = device;
          hello["session"] = token;
          respond(200, hello);
          return;
        }
        if (parsed.session != token || !incoming.begin(parsed.length, freeMemory(), largestMemory())) {
          JsonDocument error;
          error["error"] = "session-or-memory";
          message = parsed.session != token ? tr(STR_DIRECT_REJECTED) : tr(STR_DIRECT_MEMORY_ERROR);
          respond(409, error);
          requestUpdate();
          return;
        }
        headerDone = true;
      }
    } else {
      const uint8_t byte = static_cast<uint8_t>(value);
      if (!incoming.append(&byte, 1)) {
        socket.stop();
        incoming.clear();
        return;
      }
      if (incoming.size() == incoming.total()) {
        if (cancelled()) {
          fail(tr(STR_DIRECT_CANCELLED));
          return;
        }
        JsonDocument receipt;
        if (!incoming.publish(receipt, freeMemory(), largestMemory())) {
          receipt["error"] = "snapshot-or-storage";
          respond(422, receipt);
          message = tr(STR_DIRECT_RECEIVE_ERROR);
          requestUpdate();
          return;
        }
        receiptId = receipt["bookId"].as<const char*>();
        cancelled();  // A late cancellation cannot undo publication; retain the received result.
        respond(200, receipt);
        state = State::Message;
        message = tr(STR_DIRECT_RECEIVED);
        listener.end();
        MDNS.end();
        requestUpdate();
        return;
      }
    }
  }
  if (!socket.connected() && !socket.available()) {
    socket.stop();
    incoming.clear();
    header.clear();
    headerDone = false;
  }
  if (incoming.size() && now - lastProgress > 1000) {
    lastProgress = now;
    requestUpdate();
  }
}
void DirectShareActivity::loop() {
  if (consumeBackRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Back)) consumeBackRelease = false;
    return;
  }
  const bool ignore = ignoreOpeningConfirm;
  if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) ignoreOpeningConfirm = false;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.isPressed(MappedInputManager::Button::Back)) {
    if (state == State::Send || state == State::Reply || (state == State::Receive && incoming.size())) {
      RenderLock lock;
      consumeBackRelease = true;
      fail(posted && sent == bodySize ? tr(STR_DIRECT_UNCERTAIN) : tr(STR_DIRECT_CANCELLED));
      return;
    }
    finish();
    return;
  }
  if (state == State::Prepare || state == State::Discover) requestUpdateAndWait();
  if (state == State::Confirm && !ignore && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    {
      RenderLock lock;
      state = State::Send;
      requestUpdate();
    }
    requestUpdateAndWait();
    {
      RenderLock lock;
      HalPowerManager::Lock power;
      beginRequest(true);
    }
    return;
  }
  RenderLock lock;
  HalPowerManager::Lock power;
  if (state != State::Prepare && state != State::Wifi && state != State::Message && WiFi.status() != WL_CONNECTED) {
    fail(posted ? tr(STR_DIRECT_UNCERTAIN) : tr(STR_DIRECT_NETWORK_ERROR));
    return;
  }
  if (state == State::Prepare) {
    prepare();
    return;
  }
  if (state == State::Discover) {
    discover();
    return;
  }
  if (state == State::Receive) {
    receive();
    return;
  }
  if (state == State::Hello || state == State::Reply) {
    readReply();
    return;
  }
  if (state == State::Send) {
    if (cancelled()) {
      fail(sent == bodySize ? tr(STR_DIRECT_UNCERTAIN) : tr(STR_DIRECT_CANCELLED));
      return;
    }
    const auto count = std::min<size_t>(1024, bodySize - sent);
    const auto written = socket.write(body.get() + sent, count);
    if (!written || millis() - startedIo > RequestTimeout) {
      fail(tr(STR_DIRECT_UNCERTAIN));
      return;
    }
    sent += written;
    lastIo = millis();
    if (sent == bodySize) state = State::Reply;
    requestUpdate();
    return;
  }
  if (state == State::Peers) {
    const int count = static_cast<int>(peers.size() + 1);
    navigator.onNextRelease([this, count] {
      cursor = ButtonNavigator::nextIndex(cursor, count);
      requestUpdate();
    });
    navigator.onPreviousRelease([this, count] {
      cursor = ButtonNavigator::previousIndex(cursor, count);
      requestUpdate();
    });
  }
  if (ignore || !mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return;
  if (state == State::Peers) {
    if (cursor == peers.size()) {
      state = State::Discover;
      requestUpdate();
    } else
      beginRequest(false);
  }
}
void DirectShareActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto layout = UiLayout::from(renderer);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool inverted = renderer.getOrientation() == GfxRenderer::Orientation::PortraitInverted;
  const int headerY = layout.content.y + metrics.topPadding + (inverted ? metrics.buttonHintsHeight : 0);
  const int top = headerY + metrics.headerHeight + metrics.verticalSpacing;
  const int line = renderer.getLineHeight(UI_10_FONT_ID) + 8;
  const int offset = layout.content.x + layout.content.width / 2 - renderer.getScreenWidth() / 2;
  const auto text = [&](int row, const std::string& value) {
    const auto clipped = renderer.truncatedText(UI_10_FONT_ID, value.c_str(), layout.content.width - 20);
    renderer.drawCenteredTextOffset(UI_10_FONT_ID, top + line * row, clipped.c_str(), true, offset);
  };
  GUI.drawHeader(renderer, Rect{layout.content.x, headerY, layout.content.width, metrics.headerHeight},
                 receiving ? tr(STR_DIRECT_RECEIVE) : tr(STR_DIRECT_SEND));
  if (state == State::Peers) {
    text(0, peers.empty() ? tr(STR_DIRECT_NO_PEERS) : tr(STR_DIRECT_PICK_PEER));
    GUI.drawList(
        renderer,
        Rect{layout.content.x, top + line, layout.content.width,
             layout.content.height - top - 2 * line - metrics.buttonHintsHeight},
        peers.size() + 1, cursor,
        [this](int i) {
          return static_cast<size_t>(i) == peers.size() ? std::string(tr(STR_DIRECT_SEARCH)) : peers[i].name;
        },
        [this](int i) {
          return static_cast<size_t>(i) == peers.size() ? std::string("") : std::string(peers[i].ip.toString().c_str());
        });
  } else if (state == State::Confirm) {
    text(0, book.title);
    text(1, peers[cursor].name);
    text(2, peers[cursor].ip.toString().c_str());
    text(3, tr(STR_DIRECT_SEND_CONFIRM));
    text(4, tr(STR_SHARE_REPLACE_NOTICE));
    text(5, tr(STR_DIRECT_NO_IMPORT));
  } else if (state == State::Receive) {
    text(0, device);
    text(1, WiFi.localIP().toString().c_str());
    text(2, tr(STR_DIRECT_WAITING));
    text(3, tr(STR_DIRECT_SAME_WIFI));
    if (incoming.total()) text(4, std::to_string(incoming.size()) + " / " + std::to_string(incoming.total()));
    if (!message.empty()) text(5, message);
  } else if (state == State::Message) {
    text(0, message);
    if (!receiptId.empty()) {
      text(2, receiptId);
      text(3, tr(STR_DIRECT_IMPORT_LATER));
      text(4, tr(STR_DIRECT_NO_IMPORT));
    }
  } else {
    text(0, state == State::Prepare    ? tr(STR_DIRECT_PREPARING)
            : state == State::Discover ? tr(STR_DIRECT_SEARCHING)
            : state == State::Wifi     ? tr(STR_DIRECT_CONNECTING)
                                       : tr(STR_DIRECT_TRANSFERRING));
    if (state == State::Send || state == State::Reply) text(2, std::to_string(sent) + " / " + std::to_string(bodySize));
    text(4, tr(STR_SHARE_CANCEL_HINT));
  }
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK),
                            state == State::Confirm ? tr(STR_SHARE_WRITE_SHORT)
                            : state == State::Peers ? tr(STR_SELECT)
                                                    : "",
                            state == State::Peers ? tr(STR_DIR_UP) : "", state == State::Peers ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
