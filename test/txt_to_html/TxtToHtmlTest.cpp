#include <Txt.h>
#include <TxtToHtml.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>

struct Output : Print {
  std::string text;
  size_t limit = std::numeric_limits<size_t>::max();
  size_t write(const uint8_t* bytes, size_t count) override {
    count = std::min(count, limit - text.size());
    text.append(reinterpret_cast<const char*>(bytes), count);
    return count;
  }
};

std::string body(const std::string& html) {
  const auto start = html.find("<body>\n") + 7;
  return html.substr(start, html.find("\n</body>") - start);
}

std::string convert(const std::string& text) {
  Storage.files["/book.txt"] = text;
  Txt txt("/book.txt", "/cache");
  Output output;
  assert(txt.streamToHtml(output));
  return body(output.text);
}

int main() {
  assert(convert("").empty());
  assert(convert("\xef\xbb\xbf日本語\r\n\n  &<>") == "日本語<br /><br />&#160;&#160;&amp;&lt;&gt;");
  assert(convert(std::string("\x93\xfa\x96\x7b\x8c\xea\xb1", 7)) == "日本語ｱ");
  assert(convert("a  b\r\nc") == "a&#160; b<br />c");
  assert(convert("a\fb") == "a<div style=\"page-break-after:always\"></div>b");
  // Multibyte characters crossing both the detection sample and decoder chunks.
  assert(convert(std::string(1023, 'a') + "日本語" + std::string(3069, 'b') + "日") ==
         std::string(1023, 'a') + "日本語" + std::string(3069, 'b') + "日");
  assert(convert(std::string(1023, 'a') + std::string("\x93\xfa", 2)) == std::string(1023, 'a') + "日");
  assert(convert(std::string(4095, 'a') + "日") == std::string(4095, 'a') + "日");
  assert(convert(std::string(1023, 'a') + "😀") == std::string(1023, 'a') + "😀");
  assert(convert(std::string(1025, '\r') + "x") == "x");
  assert(convert(std::string(100000, 'a')) == std::string(100000, 'a'));

  // A streaming reader may split the BOM into single-byte chunks.
  struct Reader { std::string text; size_t position = 0; } reader{"\xef\xbb\xbf本文"};
  Output output;
  assert(TxtToHtml::stream("/日本&<>.TXT", &reader, [](void* context, uint8_t* bytes, size_t) {
    auto& source = *static_cast<Reader*>(context);
    if (source.position == source.text.size()) return 0;
    *bytes = source.text[source.position++];
    return 1;
  }, output));
  assert(body(output.text) == "本文");
  assert(output.text.find("<title>日本&amp;&lt;&gt;</title>") != std::string::npos);

  Output failed;
  assert(!TxtToHtml::stream("book.txt", nullptr, [](void*, uint8_t*, size_t) { return -1; }, failed));
  Output full;
  full.limit = 16;
  assert(!TxtToHtml::stream("book.txt", "content", full));
  reader.position = 0;
  Output interrupted;
  assert(!TxtToHtml::stream("book.txt", &reader, [](void* context, uint8_t* bytes, size_t size) {
    auto& source = *static_cast<Reader*>(context);
    if (source.position++ == 0) { std::memset(bytes, 'a', size); return static_cast<int>(size); }
    return -1;
  }, interrupted));
  Storage.files["/broken.txt"] = std::string(10000, 'x');
  failReadAt = 0;
  Txt brokenSample("/broken.txt", "/cache");
  Output sampleFailure;
  assert(!brokenSample.streamToHtml(sampleFailure));
  failReadAt = 5000;
  Txt brokenBody("/broken.txt", "/cache");
  Output bodyFailure;
  assert(!brokenBody.streamToHtml(bodyFailure));
  failReadAt = std::numeric_limits<size_t>::max();
  Txt missing("/missing.txt", "/cache");
  Output absent;
  assert(!missing.streamToHtml(absent));
  std::cout << "PASS: Japanese TXT streaming, boundaries, layout preservation, and I/O failures\n";
}
