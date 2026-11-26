#pragma once

#include <HalStorage.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

class Print;

class Txt {
 public:
  enum class Encoding : uint8_t { UTF8 = 0, CP932 = 1 };

  struct DecodedLine {
    std::string text;
    // Raw source offset corresponding to each UTF-8 byte in text.
    std::vector<size_t> rawEnds;
    size_t nextOffset = 0;
    bool newline = false;
    bool pageBreak = false;
  };

 private:
  std::string filepath;
  std::string cacheBasePath;
  std::string cachePath;
  bool loaded = false;
  size_t fileSize = 0;
  size_t contentOffset = 0;
  Encoding encoding = Encoding::UTF8;

 public:
  explicit Txt(std::string path, std::string cacheBasePath);

  bool load();
  [[nodiscard]] const std::string& getPath() const { return filepath; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }
  [[nodiscard]] std::string getTitle() const;
  [[nodiscard]] size_t getFileSize() const { return fileSize; }
  [[nodiscard]] size_t getContentOffset() const { return contentOffset; }
  [[nodiscard]] Encoding getEncoding() const { return encoding; }

  // Stream decoded UTF-8/CP932 text as XHTML without buffering the entire book.
  [[nodiscard]] bool streamToHtml(Print& output);
  // Publish a validated, decoded XHTML cache. Metadata/sections are discarded
  // when the source changes, while saved reader state remains separate.
  [[nodiscard]] bool prepareHtmlCache(const std::string& readerCachePath, bool buildIfMissing);
  [[nodiscard]] static bool isTextFile(std::string_view path);

  void setupCacheDir() const;

  // Cover image support - looks for cover.bmp/jpg/jpeg/png in same folder as txt file
  [[nodiscard]] std::string getCoverBmpPath() const;
  [[nodiscard]] bool generateCoverBmp() const;
  [[nodiscard]] std::string findCoverImage() const;

  // Read content from file
  [[nodiscard]] bool readContent(uint8_t* buffer, size_t offset, size_t length) const;
  [[nodiscard]] bool readDecodedLine(size_t offset, size_t maxRawBytes, DecodedLine& out) const;
  [[nodiscard]] bool readDecodedLine(FsFile& file, size_t offset, size_t maxRawBytes, DecodedLine& out) const;
};
