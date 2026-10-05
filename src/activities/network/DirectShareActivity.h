#pragma once
#include <NetworkClient.h>
#include <NetworkServer.h>

#include "activities/Activity.h"
#include "sync/DirectShareProtocol.h"
#include "sync/ShareCandidateCatalog.h"
#include "util/ButtonNavigator.h"

class DirectShareActivity final : public Activity {
  enum class State { Prepare, Wifi, Discover, Peers, Hello, Confirm, Send, Reply, Receive, Message };
  struct Peer {
    std::string name;
    IPAddress ip;
    uint16_t port;
  };
  bool receiving, ignoreOpeningConfirm = true, consumeBackRelease = false, posted = false;
  State state = State::Wifi;
  yomuka::sync::ShareCandidate book;
  ButtonNavigator navigator;
  std::vector<Peer> peers;
  size_t cursor = 0, sent = 0, bodySize = 0;
  std::unique_ptr<uint8_t[]> body;
  std::string identity, digest, device, token, header, reply, message, receiptId;
  yomuka::sync::DirectHeader parsed;
  yomuka::sync::DirectSnapshot incoming;
  NetworkServer listener{80, 1};
  NetworkClient socket;
  unsigned long lastIo = 0, startedIo = 0, lastProgress = 0;
  bool headerDone = false;
  void connectWifi();
  void connected();
  void discover();
  void prepare();
  void beginRequest(bool upload);
  void readReply();
  void receive();
  void respond(int status, const JsonDocument& doc);
  void fail(const char* text);
  bool cancelled();

 public:
  DirectShareActivity(GfxRenderer& renderer, MappedInputManager& input, bool receiving,
                      const yomuka::sync::ShareCandidate& book = {})
      : Activity("DirectShare", renderer, input), receiving(receiving), book(book) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool supportsUiLandscape() const override { return true; }
  bool preventAutoSleep() override { return state != State::Message; }
  bool skipLoopDelay() override {
    return state == State::Send || state == State::Reply || state == State::Hello || state == State::Receive;
  }
};
