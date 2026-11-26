#pragma once
#include <HalStorage.h>
#include <deque>
class ZipFile {
 public:
  struct SizeTarget { uint64_t hash; uint16_t len; uint16_t index; };
  inline static int openCalls = 0;
  inline static std::map<std::string, size_t> itemSizes;
  explicit ZipFile(const std::string&) {}
  bool open() { ++openCalls; return true; }
  void close() {}
  static uint64_t fnvHash64(const char*, size_t) { return 0; }
  int fillUncompressedSizes(const std::deque<SizeTarget>&, std::deque<uint32_t>&) { return 0; }
  bool getInflatedFileSize(const char* path, size_t* size) {
    if (!itemSizes.count(path)) return false;
    *size = itemSizes.at(path);
    return true;
  }
  uint8_t* readFileToMemory(const char*, size_t*, bool) { return nullptr; }
  bool readFileToStream(const char*, Print&, size_t) { return false; }
};
