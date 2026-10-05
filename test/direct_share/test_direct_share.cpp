#include <HalStorage.h>

#include <iostream>

#include "sync/DirectShareProtocol.h"
#include "sync/ProgressStorage.h"
#include "sync/SnapshotExchange.h"
using namespace yomuka::sync;
#define CHECK(condition)                                   \
  do {                                                     \
    if (!(condition)) {                                    \
      std::cerr << __LINE__ << ": " << #condition << "\n"; \
      return 1;                                            \
    }                                                      \
  } while (0)
int main() {
  const std::string request =
      "POST /yomuka/snapshot HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 200\r\nX-Yomuka-Session: "
      "1234567890abcdef\r\n\r\n";
  DirectHeader header;
  CHECK(parseDirectHeader(request, false, header) && header.length == 200 && header.session == "1234567890abcdef");
  CHECK(parseDirectHeader("GET /yomuka/hello HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", false, header));
  for (const auto& bad : {"POST /yomuka/snapshot HTTP/1.1\r\nContent-Length: 65537\r\n\r\n",
                          "POST /yomuka/snapshot HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n",
                          "GET /yomuka/hello HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
                          "GET /yomuka/hello HTTP/1.1\r\nContent-Length: 5\r\n\r\n", "GET /wrong HTTP/1.1\r\n\r\n",
                          "GET /yomuka/hello HTTP/1.1\r\nBad Name: x\r\n\r\n"})
    CHECK(!parseDirectHeader(bad, false, header));
  CHECK(!parseDirectHeader(std::string(2049, 'x'), false, header));
  CHECK(parseDirectHeader("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n", true, header));
  CHECK(!parseDirectHeader("HTTP/1.1 200 OK\r\nContent-Length: 513\r\n\r\n", true, header));
  const uint8_t raw[] = {0, 0, 0, 0, 10, 0, 0, 0};
  CHECK(saveProgress("/.crosspoint/books/0000000000000001/progress.bin", raw, 8));
  ReadingHistoryStore history;
  JsonDocument doc;
  CHECK(exportSnapshot({1, 2, "/a.epub", "A", ""}, Position, history, doc) == ExchangeError::None);
  std::string payload;
  serializeJson(doc, payload);
  const auto bytes = reinterpret_cast<const uint8_t*>(payload.data());
  const std::string destination = "/YomukaSync/0000000000000001.json";
  Storage.put(destination, "old snapshot");
  const auto readerState = *Storage.files["/.crosspoint/books/0000000000000001/progress.bin"];
  DirectSnapshot incoming;
  JsonDocument ack;
  CHECK(!incoming.begin(65537, 999999, 999999));
  CHECK(!incoming.begin(payload.size(), 0, 999999));
  CHECK(incoming.begin(payload.size(), 999999, 999999));
  CHECK(incoming.append(bytes, 10) && !incoming.publish(ack, 999999, 999999));
  CHECK(*Storage.files[destination] == "old snapshot");
  incoming.clear();  // cancellation
  CHECK(!incoming.publish(ack, 999999, 999999) && *Storage.files[destination] == "old snapshot");
  CHECK(incoming.begin(payload.size(), 999999, 999999));
  for (size_t i = 0; i < payload.size(); ++i) CHECK(incoming.append(bytes + i, 1));
  CHECK(incoming.publish(ack, 999999, 999999));
  CHECK(*Storage.files[destination] == payload &&
        *Storage.files["/.crosspoint/books/0000000000000001/progress.bin"] == readerState);
  CHECK(directAccepted(ack, "0000000000000001", directDigest(bytes, payload.size()), payload.size()));
  CHECK(!directAccepted(ack, "0000000000000002", directDigest(bytes, payload.size()), payload.size()));
  CHECK(!directAccepted(ack, "0000000000000001", "wrong", payload.size()));
  CHECK(incoming.begin(payload.size(), 999999, 999999));
  CHECK(incoming.append(bytes, payload.size()));
  Storage.failWrite = destination + ".tmp";
  CHECK(!incoming.publish(ack, 999999, 999999) && *Storage.files[destination] == payload);
  Storage.failWrite.clear();
  CHECK(incoming.begin(3, 999999, 999999));
  CHECK(incoming.append(reinterpret_cast<const uint8_t*>("bad"), 3));
  CHECK(!incoming.publish(ack, 999999, 999999) && *Storage.files[destination] == payload);
  CHECK(incoming.begin(3, 999999, 999999));
  CHECK(!incoming.append(bytes, 4));
  JsonDocument hello;
  hello["protocol"] = "yomuka-direct";
  hello["version"] = 1;
  hello["device"] = "X3-abcdef";
  hello["session"] = "1234567890abcdef";
  std::string device, session;
  CHECK(directHello(hello, device, session));
  hello["version"] = 2;
  CHECK(!directHello(hello, device, session));
  std::cout << "PASS: bounded HTTP framing, handshake and ACK, complete-only publication, cancellation and old-file "
               "retention\n";
}
