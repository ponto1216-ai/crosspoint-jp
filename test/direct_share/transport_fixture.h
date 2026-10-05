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
  enum class State { Hello, Confirm, Reply, Receive, Message };
  State state = State::Receive;
  bool posted = false, headerDone = false, cancel = false;
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
  std::cout << "PASS: actual receive/reply loops, handshake, truncated POST, idle timeout, session rejection, "
               "cancellation, failed save, ACK loss\n";
  return 0;
}
