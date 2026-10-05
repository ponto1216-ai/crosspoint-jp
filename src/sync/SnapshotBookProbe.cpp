#include "SnapshotBookProbe.h"

#include <Print.h>
#include <ZipFile.h>
#include <expat.h>

#include <algorithm>
#include <cstring>
#include <vector>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

namespace yomuka::sync {
namespace {
const char* local(const char* name) {
  const auto colon = std::strchr(name, ':');
  return colon ? colon + 1 : name;
}
const char* attribute(const char** attrs, const char* key) {
  for (size_t i = 0; attrs[i]; i += 2)
    if (std::strcmp(attrs[i], key) == 0) return attrs[i + 1];
  return "";
}
class ProbeXml final : public Print {
  XML_Parser parser = nullptr;
  size_t remaining;
  bool container;
  const std::function<bool()>& cancel;
  unsigned depth = 0, manifestDepth = 0, spineDepth = 0, rootsDepth = 0;
  bool valid = true, rootSeen = false, spineSeen = false;
  size_t idBytes = 0;
  std::vector<std::string> ids;
  void reject() {
    valid = false;
    XML_StopParser(parser, XML_FALSE);
  }
  static void start(void* context, const char* name, const char** attrs) {
    auto& self = *static_cast<ProbeXml*>(context);
    ++self.depth;
    if (self.depth > 64) return self.reject();
    const auto tag = local(name);
    if (self.depth == 1) {
      self.rootSeen = std::strcmp(tag, self.container ? "container" : "package") == 0;
      if (!self.rootSeen) self.reject();
    }
    if (self.container) {
      if (self.depth == 2 && std::strcmp(tag, "rootfiles") == 0) self.rootsDepth = self.depth;
      if (self.rootsDepth && self.depth == self.rootsDepth + 1 && std::strcmp(tag, "rootfile") == 0 &&
          std::strcmp(attribute(attrs, "media-type"), "application/oebps-package+xml") == 0) {
        const char* path = attribute(attrs, "full-path");
        if (!*path || std::strlen(path) > 512) return self.reject();
        self.opf = path;
      }
      return;
    }
    if (self.depth == 2 && std::strcmp(tag, "manifest") == 0) self.manifestDepth = self.depth;
    if (self.depth == 2 && std::strcmp(tag, "spine") == 0) {
      if (self.spineSeen) return self.reject();
      self.spineSeen = true;
      self.spineDepth = self.depth;
    }
    if (self.manifestDepth && self.depth == self.manifestDepth + 1 && std::strcmp(tag, "item") == 0) {
      const char* id = attribute(attrs, "id");
      const char* href = attribute(attrs, "href");
      const size_t length = std::strlen(id);
      if (!length || length > 256 || !*href || self.ids.size() >= 4096 || self.idBytes + length > 131072 ||
          std::find(self.ids.begin(), self.ids.end(), id) != self.ids.end())
        return self.reject();
#if defined(ARDUINO_ARCH_ESP32)
      // Growing a vector doubles its capacity; retain headroom for that allocation.
      const size_t needed = (self.ids.size() + 1) * sizeof(std::string) * 2 + 8192;
      if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < needed + 32768 ||
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < needed)
        return self.reject();
#endif
      self.ids.emplace_back(id);
      self.idBytes += length;
    }
    if (self.spineDepth && self.depth == self.spineDepth + 1 && std::strcmp(tag, "itemref") == 0) {
      const char* id = attribute(attrs, "idref");
      if (!*id || std::find(self.ids.begin(), self.ids.end(), id) == self.ids.end() || self.spines >= 65535)
        return self.reject();
      ++self.spines;  // The reader also includes linear="no" entries.
    }
  }
  static void end(void* context, const char*) {
    auto& self = *static_cast<ProbeXml*>(context);
    if (self.depth == self.manifestDepth) self.manifestDepth = 0;
    if (self.depth == self.spineDepth) self.spineDepth = 0;
    if (self.depth == self.rootsDepth) self.rootsDepth = 0;
    --self.depth;
  }
  static void doctype(void* context, const char*, const char*, const char*, int) {
    static_cast<ProbeXml*>(context)->reject();
  }

 public:
  std::string opf;
  uint32_t spines = 0;
  ProbeXml(size_t size, bool container, const std::function<bool()>& cancel)
      : remaining(size), container(container), cancel(cancel) {
    parser = XML_ParserCreate(nullptr);
    if (!parser) {
      valid = false;
      return;
    }
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, start, end);
    XML_SetStartDoctypeDeclHandler(parser, doctype);
  }
  ~ProbeXml() override {
    if (parser) XML_ParserFree(parser);
  }
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    if (!valid || !size || size > remaining) return 0;
    if (cancel && cancel()) {
      valid = false;
      return 0;
    }
    remaining -= size;
    if (XML_Parse(parser, reinterpret_cast<const char*>(data), static_cast<int>(size), remaining == 0) !=
        XML_STATUS_OK) {
      valid = false;
      return 0;
    }
    return size;
  }
  bool complete() const { return valid && !remaining && rootSeen && !depth; }
};
}  // namespace
bool probeSnapshotBook(const std::string& path, uint64_t expectedId, uint32_t& spines,
                       const std::function<bool()>& cancel) {
  ZipFile zip(path);
  uint64_t actual = 0;
  if (!expectedId || !zip.getArchiveFingerprint(&actual) || actual != expectedId) return false;
  size_t size = 0;
  if (!zip.getInflatedFileSize("META-INF/container.xml", &size) || !size || size > 65536) return false;
  std::string opf;
  {
    ProbeXml xml(size, true, cancel);
    if (!zip.readFileToStream("META-INF/container.xml", xml, 1024) || !xml.complete() || xml.opf.empty()) return false;
    opf = xml.opf;
  }
  if (!zip.getInflatedFileSize(opf.c_str(), &size) || !size || size > 1048576) return false;
  ProbeXml xml(size, false, cancel);
  if (!zip.readFileToStream(opf.c_str(), xml, 1024) || !xml.complete() || !xml.spines) return false;
  spines = xml.spines;
  return true;
}
}  // namespace yomuka::sync
