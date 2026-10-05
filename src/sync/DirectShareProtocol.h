#pragma once
#include <ArduinoJson.h>

#include <memory>
#include <string>

namespace yomuka::sync {
constexpr size_t DirectSnapshotLimit = 65536;
struct DirectHeader {
  std::string method, path, session;
  size_t length = 0;
  int status = 0;
};
bool parseDirectHeader(const std::string& text, bool response, DirectHeader& output);
std::string directDigest(const uint8_t* bytes, size_t size);
bool directHello(const JsonDocument& doc, std::string& device, std::string& session);
bool directAccepted(const JsonDocument& doc, const std::string& id, const std::string& digest, size_t size);
// Admission is checked before allocating; incomplete/cancelled data never reaches SD.
class DirectSnapshot {
  std::unique_ptr<uint8_t[]> bytes;
  size_t expected = 0, used = 0;

 public:
  bool begin(size_t size, size_t free, size_t largest);
  bool append(const uint8_t* data, size_t count);
  size_t size() const { return used; }
  size_t total() const { return expected; }
  void clear();
  bool publish(JsonDocument& receipt, size_t free, size_t largest);
};
}  // namespace yomuka::sync
