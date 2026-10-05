#include "SnapshotComparison.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>

#include "BookReaderSettings.h"
#include "BookmarkStorage.h"
#include "ProgressStorage.h"
#include "ReadingHistoryStore.h"
#include "SnapshotPreview.h"
#include "SnapshotValidation.h"
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif
namespace yomuka::sync {
namespace {
struct FileReader {
  FsFile& file;
  size_t expected;
  std::array<uint8_t, 512> buffer{};
  size_t offset = 0, count = 0;
  bool failed = false;
  int read() {
    if (offset == count) {
      if (file.position() >= expected) return -1;
      const auto wanted = std::min(buffer.size(), expected - file.position());
      const int received = file.read(buffer.data(), wanted);
      if (received <= 0) {
        failed = true;
        return -1;
      }
      count = static_cast<size_t>(received);
      offset = 0;
    }
    return buffer[offset++];
  }
  size_t readBytes(char* data, size_t length) {
    size_t copied = 0;
    while (copied < length) {
      const int byte = read();
      if (byte < 0) break;
      data[copied++] = static_cast<char>(byte);
    }
    return copied;
  }
};
bool readDocument(const std::string& path, JsonDocument& document, size_t limit = 262144) {
  auto file = Storage.open(path.c_str());
  if (!file || file.isDirectory() || !file.size() || file.size() > limit) return false;
#if defined(ARDUINO_ARCH_ESP32)
  if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < file.size() * 3 + 32768) return false;
#endif
  FileReader reader{file, file.size()};
  bool valid = !deserializeJson(document, reader) && !document.overflowed();
  // Reject an incomplete read or trailing non-JSON bytes, just like normal sync reads.
  if (valid) {
    int byte = 0;
    while ((byte = reader.read()) >= 0) {
      if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') valid = false;
    }
  }
  return file.close() && valid && !reader.failed;
}

struct Summary {
  bool present = false, valid = true;
  uint32_t date = 0;
  std::string value = "なし";
};
bool absent(const std::string& path, Summary& out) {
  if (Storage.exists(path.c_str())) return false;
  out.valid = !Storage.exists((path + ".bak").c_str()) && !Storage.exists((path + ".tmp").c_str());
  return true;
}
bool readSmall(const std::string& path, uint8_t* bytes, size_t capacity, size_t& size) {
  auto file = Storage.open(path.c_str());
  if (!file || file.isDirectory()) return false;
  size = file.size();
  if (!size || size > capacity || file.read(bytes, size) != static_cast<int>(size)) return false;
  return file.close();
}
std::string position(uint32_t spine, uint32_t page, uint32_t count, bool knownPercent, uint32_t percent) {
  std::string text =
      spine == count ? "本の終端" : "章 " + std::to_string(spine + 1) + " / 頁 " + std::to_string(page + 1);
  return text + " / " + (knownPercent ? std::to_string(percent) + "%" : "進捗不明");
}
unsigned bits(unsigned value) {
  unsigned n = 0;
  for (; value; value >>= 1) n += value & 1;
  return n;
}
std::string duration(uint32_t seconds, uint32_t sessions) {
  return std::to_string(seconds / 3600) + "時間" + std::to_string(seconds / 60 % 60) + "分" +
         std::to_string(seconds % 60) + "秒 / " + std::to_string(sessions) + "回";
}
Summary localSummary(unsigned unit, const ReceivedBook& book, const std::string& id) {
  Summary out;
  const auto base = "/.crosspoint/books/" + id;
  if (unit == 0) {
    const auto path = base + "/progress.bin";
    if (absent(path, out)) return out;
    out.present = true;
    uint8_t bytes[8]{}, metadata[30]{};
    size_t size = 0, n = 0;
    Progress progress;
    out.valid = readSmall(path, bytes, sizeof(bytes), size) &&
                decodeLegacyProgress(bytes, size, progress) == ProgressError::None &&
                validateProgressForBook(progress, book.spines) == ProgressError::None;
    const auto timePath = path + ".sync-time";
    Summary time;
    if (!absent(timePath, time))
      out.valid = out.valid && readSmall(timePath, metadata, sizeof(metadata), n) &&
                  decodeSavedProgressTime(bytes, size, metadata, n, out.date);
    else if (!time.valid)
      out.valid = false;
    if (out.valid)
      out.value = position(progress.spineIndex, progress.chapterPage, book.spines, progress.percent.has_value(),
                           progress.percent.value_or(0));
    return out;
  }
  const std::string path = unit == 1   ? base + "/bookmarks.json"
                           : unit == 2 ? "/.crosspoint/book-reader-settings.json"
                                       : "/.crosspoint/reading-history.json";
  if (absent(path, out)) return out;
  JsonDocument doc;
  if (!readDocument(path, doc, unit == 1 ? 65536 : 262144)) {
    out.valid = false;
    return out;
  }
  if (unit == 1) {
    std::vector<BookmarkEntry> bookmarks;
    out.present = true;
    out.valid = decodeSavedBookmarks(doc.as<JsonObjectConst>(), bookmarks, out.date);
    out.value = std::to_string(bookmarks.size()) + "件";
  } else if (unit == 2) {
    if (!doc["formatVersion"].is<uint8_t>() || doc["formatVersion"] != 1 || !doc["books"].is<JsonObjectConst>()) {
      out.valid = false;
      return out;
    }
    if (doc["books"][id].isUnbound()) return out;
    BookReaderSettings::Override settings;
    out.present = true;
    out.valid = BookReaderSettings::decodeSavedOverride(doc["books"][id].as<JsonObjectConst>(), settings, out.date);
    out.value =
        std::to_string(bits(settings.fields) + bits(settings.horizontal.fields) + bits(settings.vertical.fields)) +
        "項目";
  } else {
    std::vector<ReadingHistoryBook> records;
    std::vector<uint64_t> removed;
    out.valid = ReadingHistoryStore::decodeSavedBooks(doc, records, removed) == ReadStatus::Present;
    if (!out.valid) return out;
    for (const auto& record : records)
      if (record.bookId == book.id) {
        if (out.present) {
          out.valid = false;
          return out;
        }
        out.present = true;
        out.date = record.updatedAt;
        out.value = duration(record.seconds, record.sessionCount);
      }
    if (std::find(removed.begin(), removed.end(), book.id) != removed.end()) {
      if (out.present) {
        out.valid = false;
        return out;
      }
      out.present = true;
      out.date = doc["removedBooks"][id];
      out.value = duration(0, 0);
    }
  }
  return out;
}
std::string incomingSummary(unsigned unit, JsonVariantConst data, uint32_t spines) {
  if (unit == 0)
    return position(data["spineIndex"], data["chapterPage"], spines, !data["percent"].isNull(), data["percent"]);
  if (unit == 1) return std::to_string(data.size()) + "件";
  if (unit == 2) {
    unsigned n = 0;
    for (JsonPairConst entry : data.as<JsonObjectConst>()) {
      const auto key = std::string(entry.key().c_str());
      n += key == "horizontal" || key == "vertical" ? entry.value().size() : 1;
    }
    return std::to_string(n) + "項目";
  }
  return duration(data["seconds"], data["sessionCount"]);
}
std::string date(uint32_t value) {
  if (!value) return "日時不明";
  const time_t stamp = value;
  struct tm local{};
  char text[32]{};
  if (!localtime_r(&stamp, &local) || !strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local)) return "日時不明";
  return text;
}
}  // namespace
std::string compareUpdateTimes(uint32_t local, uint32_t incoming) {
  if (!local || !incoming) return "日時不明・新旧を判定しません";
  if (incoming < local) return "注意: 受信の更新日時が古い";
  if (incoming > local) return "受信の更新日時が新しい";
  return "更新日時は同じ（内容一致とは限りません）";
}
std::vector<std::string> compareReceivedBook(const ReceivedBook& book) {
  std::vector<std::string> lines{book.title, "端末と受信ファイルの比較"};
  char key[17];
  snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(book.id));
  JsonDocument incoming;
  if (!Storage.ready() || !readDocument(book.file, incoming, 65536) ||
      validateSnapshotDocument(incoming) != SnapshotError::None ||
      validateSnapshotTarget(incoming, key, book.spines) != SnapshotError::None) {
    lines.emplace_back("受信ファイルを比較できません");
    return lines;
  }
  constexpr const char* names[] = {"progress", "bookmarks", "readerSettings", "history"};
  constexpr const char* labels[] = {"読書位置（近似）", "しおり（全体置換）", "本の設定（全体置換）",
                                    "履歴（本別の要約）"};
  for (unsigned unit = 0; unit < 4; ++unit) {
    auto next = incoming["units"][names[unit]];
    lines.emplace_back(labels[unit]);
    if (next.isUnbound()) {
      lines.emplace_back("受信にない種類・端末のデータを保持");
      continue;
    }
    const auto current = localSummary(unit, book, key);
    const uint32_t updated = next["updatedAt"];
    lines.push_back("端末: " + (current.valid ? current.value : "読めません（不明）"));
    lines.push_back("受信: " + incomingSummary(unit, next["data"], book.spines));
    lines.push_back("端末の更新: " + (current.valid && current.present ? date(current.date) : "不明"));
    lines.push_back("受信の更新: " + date(updated));
    lines.push_back(!current.valid     ? "注意: 端末データを比較できません"
                    : !current.present ? "端末にこの種類のデータなし"
                                       : compareUpdateTimes(current.date, updated));
    if (unit == 1 && next["data"].size() == 0) lines.emplace_back("注意: 現在のしおりをすべて削除");
    if (unit == 2 && next["data"].size() == 0) lines.emplace_back("注意: 本の設定をすべて解除");
  }
  lines.emplace_back("件数・日時だけで内容一致は判定しません");
  return lines;
}
}  // namespace yomuka::sync
