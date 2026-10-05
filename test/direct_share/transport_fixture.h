#define LOG_INF(...) ((void)0)
#include "sync/BatchSnapshot.h"
#include "sync/DirectShareBatch.h"
// The runner inserts the actual four transport methods into this socket/SD fixture.
struct Channel {
  std::string input, output;
  size_t at = 0;
  bool active = true;
};
struct Socket {
  std::shared_ptr<Channel> data;
  explicit operator bool() const { return data && data->active; }
  bool connected() const { return static_cast<bool>(*this); }
  int available() const { return data ? static_cast<int>(data->input.size() - data->at) : 0; }
  int read() { return available() ? static_cast<unsigned char>(data->input[data->at++]) : -1; }
  size_t write(const uint8_t* p, size_t n) {
    data->output.append(reinterpret_cast<const char*>(p), n);
    return n;
  }
  void stop() {
    if (data) data->active = false;
  }
  void setTimeout(unsigned) {}
};
struct Listener {
  Socket pending;
  bool hasClient() const { return static_cast<bool>(pending); }
  Socket accept() {
    auto s = pending;
    pending = {};
    return s;
  }
  void end() {}
};
struct Mdns {
  void end() {}
} MDNS;
constexpr unsigned long IdleTimeout = 10000, RequestTimeout = 30000;
size_t freeMemory() { return 999999; }
size_t largestMemory() { return 999999; }
class DirectShareActivity {
 public:
  enum class State { Prepare, Hello, Confirm, Reply, Receive, Message };
  State state = State::Receive;
  bool posted = false, headerDone = false, cancel = false;
  bool batchStarted = false, consumeBackRelease = false, preparingOffline = false;
  bool wifiStarted = false;
  std::vector<int> books;
  std::vector<uint8_t> prepared;
  std::vector<std::string> preparationNotes;
  struct OfflineBatch {
    std::vector<BatchEntry> entries;
    size_t cursor = 0;
    size_t completed() const { return cursor; }
    bool done() const { return cursor == entries.size(); }
    void step(const std::vector<int>&, ReadingHistoryStore&, const std::function<bool()>& cancel) {
      if (done()) return;
      if (cancel()) {
        while (!done()) entries[cursor++].result = BatchResult::Cancelled;
      } else
        ++cursor;
    }
    const auto& results() const { return entries; }
  } offlineBatch;
  void connectWifi() { wifiStarted = true; }
  static const char* preparationMessage(BatchError);
  void finishPreparation(bool);
  void prepareOffline();
  DirectShareBatch transfer;
  std::vector<std::string> transferNotes;
  std::unique_ptr<uint8_t[]> body;
  size_t sent = 0, receivedCount = 0;
  size_t cursor = 0, bodySize = 0;
  struct Peer {
    std::string name;
  };
  std::vector<Peer> peers{{"peer"}};
  std::string header, reply, message, identity, digest, token = "1234567890abcdef", device = "X3-abcdef", receiptId;
  DirectHeader parsed;
  DirectSnapshot incoming;
  Socket socket;
  Listener listener;
  unsigned long lastIo = 0, startedIo = 0, lastProgress = 0;
  bool cancelled() { return cancel; }
  void requestUpdate() {}
  void fail(const char*);
  void readReply();
  void respond(int, const JsonDocument&);
  void receive();
};
int transportTests(const std::string& payload) {
  const std::string target = "/YomukaSync/0000000000000001.json";
  auto request = [&](const std::string& token, const std::string& body) {
    return "POST /yomuka/snapshot HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(payload.size()) + "\r\nX-Yomuka-Session: " + token + "\r\n\r\n" + body;
  };
  auto feed = [](DirectShareActivity& a, const std::string& input) {
    auto c = std::make_shared<Channel>();
    c->input = input;
    a.listener.pending.data = c;
    return c;
  };
  auto drain = [](DirectShareActivity& a, const std::shared_ptr<Channel>& c) {
    for (unsigned i = 0; i < 100 && c->active && c->at < c->input.size(); ++i) a.receive();
  };
  Storage.nowMs = 0;
  DirectShareActivity receiver;
  auto hello = feed(receiver, "GET /yomuka/hello HTTP/1.1\r\n\r\n");
  drain(receiver, hello);
  CHECK(hello->output.find("HTTP/1.1 200") == 0);
  DirectShareActivity sender;
  sender.state = DirectShareActivity::State::Hello;
  sender.socket.data = std::make_shared<Channel>();
  sender.socket.data->input = hello->output;
  sender.readReply();
  CHECK(sender.state == DirectShareActivity::State::Confirm && sender.token == receiver.token);
  Storage.put(target, "old");
  auto partial = feed(receiver, request(receiver.token, payload.substr(0, 10)));
  drain(receiver, partial);
  CHECK(*Storage.files[target] == "old" && receiver.incoming.size() == 10);
  partial->active = false;
  receiver.receive();
  CHECK(receiver.incoming.size() == 0 && *Storage.files[target] == "old");
  auto timed = feed(receiver, request(receiver.token, payload.substr(0, 10)));
  drain(receiver, timed);
  Storage.nowMs = 10001;
  receiver.receive();
  CHECK(!timed->active && receiver.incoming.size() == 0 && *Storage.files[target] == "old");
  Storage.nowMs = 0;
  auto wrong = feed(receiver, request("0000000000000000", payload));
  drain(receiver, wrong);
  CHECK(wrong->output.find("HTTP/1.1 409") == 0 && *Storage.files[target] == "old");
  receiver.cancel = true;
  auto cancelled = feed(receiver, request(receiver.token, payload));
  drain(receiver, cancelled);
  CHECK(receiver.message == "STR_DIRECT_CANCELLED" && *Storage.files[target] == "old");
  receiver.cancel = false;
  receiver.state = DirectShareActivity::State::Receive;
  Storage.failWrite = target + ".tmp";
  auto failed = feed(receiver, request(receiver.token, payload));
  drain(receiver, failed);
  CHECK(failed->output.find("HTTP/1.1 422") == 0 && *Storage.files[target] == "old");
  Storage.failWrite.clear();
  auto saved = feed(receiver, request(receiver.token, payload));
  drain(receiver, saved);
  CHECK(saved->output.find("HTTP/1.1 200") == 0 && receiver.message == "STR_DIRECT_RECEIVED" &&
        *Storage.files[target] == payload);
  sender.posted = true;
  sender.headerDone = false;
  sender.header.clear();
  sender.reply.clear();
  sender.bodySize = payload.size();
  sender.identity = "0000000000000001";
  sender.digest = directDigest(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  sender.socket.data = std::make_shared<Channel>();
  sender.socket.data->input = saved->output;
  sender.readReply();
  CHECK(sender.message == "STR_DIRECT_SENT");
  sender.headerDone = false;
  sender.header.clear();
  sender.reply.clear();
  sender.socket.data = std::make_shared<Channel>();
  sender.socket.data->active = false;
  sender.readReply();
  CHECK(sender.message == "STR_DIRECT_UNCERTAIN");
  sender.headerDone = false;
  sender.header.clear();
  sender.reply.clear();
  sender.socket.data = std::make_shared<Channel>();
  sender.socket.data->input = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nbad";
  sender.readReply();
  CHECK(sender.message == "STR_DIRECT_UNCERTAIN");
  CHECK(receiver.receivedCount == 1 && receiver.state == DirectShareActivity::State::Receive);
  std::string second = payload;
  second.replace(second.find("0000000000000001"), 16, "0000000000000002");
  auto next = feed(receiver, request(receiver.token, second));
  drain(receiver, next);
  CHECK(next->output.find("HTTP/1.1 200") == 0 && receiver.receivedCount == 2);
  CHECK(*Storage.files[target] == payload && *Storage.files["/YomukaSync/0000000000000002.json"] == second);
  // Drive the actual sender completion method across a partial failure and cancellation.
  DirectShareActivity batchSender;
  batchSender.batchStarted = true;
  batchSender.transfer.start(4);
  batchSender.transferNotes.assign(4, "");
  batchSender.fail("STR_DIRECT_SENT");
  CHECK(batchSender.transfer.current() == 1 && batchSender.state == DirectShareActivity::State::Prepare);
  batchSender.fail("STR_DIRECT_RECEIVE_ERROR");
  CHECK(batchSender.transfer.current() == 2);
  batchSender.fail("STR_DIRECT_UNCERTAIN");
  CHECK(batchSender.transfer.current() == 3);
  batchSender.fail("STR_DIRECT_SENT");
  CHECK(batchSender.transfer.done() && batchSender.transfer.count(TransferResult::Sent) == 2);
  batchSender.transfer.start(3);
  batchSender.transferNotes.assign(3, "");
  batchSender.fail("STR_DIRECT_SENT");
  batchSender.consumeBackRelease = true;
  batchSender.fail("STR_DIRECT_UNCERTAIN");
  CHECK(batchSender.transfer.done() && batchSender.transfer.results()[0] == TransferResult::Sent &&
        batchSender.transfer.results()[1] == TransferResult::Uncertain &&
        batchSender.transfer.results()[2] == TransferResult::NotSent);
  batchSender.fail("STR_DIRECT_CANCELLED");
  CHECK(batchSender.transfer.results()[0] == TransferResult::Sent);
  CHECK(*Storage.files[target] == payload);
  batchSender.transfer.start(3);
  batchSender.transferNotes.assign(3, "");
  batchSender.fail("STR_DIRECT_SENT");  // A late Back retains the acknowledged book, stopping only later books.
  CHECK(batchSender.transfer.results()[0] == TransferResult::Sent &&
        batchSender.transfer.count(TransferResult::NotSent) == 2);
  batchSender.transfer.start(2);
  batchSender.transferNotes.assign(2, "");
  batchSender.fail("STR_DIRECT_CANCELLED");
  CHECK(batchSender.transfer.results()[0] == TransferResult::Cancelled &&
        batchSender.transfer.results()[1] == TransferResult::NotSent);
  std::cout << "PASS: actual receive/reply loops, handshake, truncated POST, idle timeout, session rejection, "
               "cancellation, failed save, ACK loss\n";
  DirectShareActivity sequential;
  sequential.books = {1, 2};
  sequential.preparingOffline = true;
  sequential.offlineBatch.entries = {{0, BatchResult::Saved, BatchError::None},
                                     {1, BatchResult::Saved, BatchError::None}};
  sequential.prepareOffline();
  CHECK(!sequential.wifiStarted && sequential.offlineBatch.completed() == 1);
  sequential.prepareOffline();
  CHECK(sequential.wifiStarted && sequential.prepared == std::vector<uint8_t>({1, 1}));
  DirectShareActivity offline;
  offline.books = {1, 2, 3};
  offline.offlineBatch.entries = {{0, BatchResult::Saved, BatchError::None},
                                  {1, BatchResult::Failed, BatchError::Source},
                                  {2, BatchResult::Saved, BatchError::None}};
  offline.finishPreparation(false);
  CHECK(offline.wifiStarted && offline.prepared == std::vector<uint8_t>({1, 0, 1}) &&
        offline.preparationNotes[1] == "STR_SHARE_SOURCE_ERROR");
  DirectShareActivity stopped;
  stopped.books = {1, 2};
  stopped.offlineBatch.entries = {{0, BatchResult::Saved, BatchError::None},
                                  {1, BatchResult::Cancelled, BatchError::None}};
  stopped.finishPreparation(true);
  CHECK(!stopped.wifiStarted && stopped.transfer.done() && stopped.transfer.count(TransferResult::NotSent) == 2);
  DirectShareActivity invalid;
  invalid.books = {1};
  invalid.offlineBatch.entries = {{0, BatchResult::Failed, BatchError::Data}};
  invalid.finishPreparation(false);
  CHECK(!invalid.wifiStarted && invalid.transfer.count(TransferResult::Failed) == 1 &&
        invalid.transferNotes[0] == "STR_SHARE_DATA_ERROR");
  return 0;
}
