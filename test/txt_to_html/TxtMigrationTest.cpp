#include <HalStorage.h>
#include <Txt.h>
#include <util/TxtProgressMigration.h>
#include <sync/ProgressStorage.h>
#include <activities/reader/ProgressFile.h>

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using TxtProgressMigration::Result;
const std::string bookPath = "/books/日本語.txt";
const std::string readerCache = "/cache/epub_new";
const std::string destination = readerCache + "/progress.bin";

void append32(std::string& output, uint32_t value) {
  for (int i = 0; i < 4; ++i) output.push_back(static_cast<char>(value >> (i * 8)));
}

std::string legacyPath() { return Txt(bookPath, "/cache").getCachePath() + "/progress.bin"; }
std::string indexPath() { return Txt(bookPath, "/cache").getCachePath() + "/index.bin"; }

std::string legacyRecord(uint32_t page, size_t length, bool finished = false, uint8_t percent = 255) {
  std::string data;
  append32(data, page);
  if (length >= 5) data.push_back(finished ? 1 : 0);
  if (length == 6) data.push_back(static_cast<char>(percent));
  return data;
}

std::string indexRecord(const std::vector<uint32_t>& offsets, uint32_t size = 100) {
  std::string data;
  append32(data, 0x54585449);
  data.push_back(6);
  append32(data, size);
  append32(data, 400);  // viewport width
  append32(data, 20);   // lines per page
  append32(data, UINT32_MAX);  // negative SD font ID is supported
  append32(data, 5);
  data.push_back(0);
  append32(data, offsets.size());
  assert(data.size() == 30);
  for (auto offset : offsets) append32(data, offset);
  return data;
}

void reset() {
  Storage = {};
  Storage.files[bookPath] = std::string(100, 'a');
  Storage.mkdir(readerCache.c_str());
  failReadAt = std::numeric_limits<size_t>::max();
  yomuka::sync::failedRecovery.clear();
  yomuka::sync::interruptedWrite = false;
}

Result migrate() { return TxtProgressMigration::migrate(bookPath, "/cache", readerCache, 1); }
uint16_t le16(const std::string& data, size_t offset) {
  return static_cast<uint8_t>(data[offset]) | (static_cast<uint8_t>(data[offset + 1]) << 8);
}
int reflowedPage(const std::string& data, int pageCount) {
  struct TestSection { int currentPage; int pageCount; };
  auto section = std::make_unique<TestSection>(TestSection{le16(data, 2), pageCount});
  int cachedChapterTotalPageCount = le16(data, 4);
  int cachedSpineIndex = le16(data, 0);
  int currentSpineIndex = 0;
#include "TxtMigrationReflow.inc"
  return section->currentPage;
}
void checkPosition(double fraction) {
  const auto& data = Storage.files.at(destination);
  assert(data.size() == 8 && le16(data, 0) == 0 && le16(data, 4) == UINT16_MAX && data[6] == 0);
  assert(le16(data, 2) != UINT16_MAX);
  assert(std::abs(le16(data, 2) / 65535.0 - fraction) < 0.00002);
  uint8_t restored[7] = {};
  assert(ProgressFile::readLegacyCompatible(destination, restored) == 7);
  assert(std::memcmp(restored, data.data(), 7) == 0);
  // Exercise the reader's actual reflow code with the migrated record.
  assert(std::abs(reflowedPage(data, 100) - static_cast<int>(fraction * 100)) <= 1);
}

int main() {
  reset();
  assert(migrate() == Result::Unchanged);
  assert(!Storage.files.count(destination));

  for (size_t length : {4u, 5u, 6u}) {
    reset();
    const auto old = legacyRecord(2, length, false, 90);
    const auto index = indexRecord({0, 20, 75});
    Storage.files[legacyPath()] = old;
    Storage.files[indexPath()] = index;
    assert(migrate() == Result::Migrated);
    checkPosition(0.75);  // source offsets take precedence over the saved percent
    assert(static_cast<uint8_t>(Storage.files.at(destination)[7]) == 75);
    assert(Storage.files.at(legacyPath()) == old && Storage.files.at(indexPath()) == index);
    const auto migrated = Storage.files.at(destination);
    Storage.files[legacyPath()] = legacyRecord(0, 6, false, 0);
    assert(migrate() == Result::Unchanged && Storage.files.at(destination) == migrated);
  }

  for (size_t length : {5u, 6u}) {
    reset();
    Storage.files[legacyPath()] = legacyRecord(UINT32_MAX, length, true, 100);
    assert(migrate() == Result::Migrated);
    const auto& data = Storage.files.at(destination);
    assert(le16(data, 0) == 1 && le16(data, 2) == 0 && le16(data, 4) == 0 && data[6] == 1 && data[7] == 100);
  }

  reset();
  Storage.files[legacyPath()] = legacyRecord(1234, 6, false, 42);
  assert(migrate() == Result::Migrated);
  checkPosition(0.42);

  reset();
  Storage.files[legacyPath()] = legacyRecord(1, 6, false, 100);
  assert(migrate() == Result::Migrated);
  assert(le16(Storage.files.at(destination), 2) == UINT16_MAX - 1 && Storage.files.at(destination)[6] == 0);

  for (const auto& badIndex : {indexRecord({0, 20, 10}), indexRecord({0, 101}), indexRecord({0, 20}, 99),
                               indexRecord({0, 20}).substr(0, 32), indexRecord({1, 20})}) {
    reset();
    Storage.files[legacyPath()] = legacyRecord(1, 6, false, 30);
    Storage.files[indexPath()] = badIndex;
    assert(migrate() == Result::Migrated);
    checkPosition(0.30);
  }
  reset();
  Storage.files[legacyPath()] = legacyRecord(1, 4);
  Storage.files[indexPath()] = indexRecord({0, 0});
  assert(migrate() == Result::NoPosition && !Storage.files.count(destination));

  reset();
  Storage.files[legacyPath()] = legacyRecord(0, 4);
  assert(migrate() == Result::Migrated);
  checkPosition(0);

  reset();
  Storage.files[bookPath] = std::string(7000100, 'a');
  std::vector<uint32_t> manyPages(70001);
  for (size_t i = 0; i < manyPages.size(); ++i) manyPages[i] = i * 100;
  Storage.files[legacyPath()] = legacyRecord(70000, 4);
  Storage.files[indexPath()] = indexRecord(manyPages, 7000100);
  assert(migrate() == Result::Migrated);
  checkPosition(7000000.0 / 7000100);

  reset();
  Storage.files[bookPath] = "\xef\xbb\xbf" + std::string(100, 'a');
  Storage.files[legacyPath()] = legacyRecord(1, 4);
  Storage.files[indexPath()] = indexRecord({3, 53}, 103);
  assert(migrate() == Result::Migrated);
  checkPosition(0.5);

  for (const auto& invalid : {std::string("bad"), std::string(7, 'a'),
                              legacyRecord(1, 6, false, 254)}) {
    reset();
    Storage.files[legacyPath()] = invalid;
    assert(migrate() == Result::NoPosition && !Storage.files.count(destination));
    assert(Storage.files.at(legacyPath()) == invalid);
  }

  reset();
  Storage.files[legacyPath()] = legacyRecord(1, 6, false, 42);
  yomuka::sync::interruptedWrite = true;
  assert(migrate() == Result::Failed && !Storage.files.count(destination));
  assert(Storage.files.count(destination + ".tmp"));
  yomuka::sync::interruptedWrite = false;
  assert(migrate() == Result::Migrated);
  checkPosition(0.42);
  const auto saved = Storage.files.at(destination);
  assert(migrate() == Result::Unchanged && Storage.files.at(destination) == saved);

  reset();
  Storage.files[legacyPath()] = legacyRecord(1, 6, false, 42);
  failReadAt = 0;
  assert(migrate() == Result::Failed && !Storage.files.count(destination));
  failReadAt = std::numeric_limits<size_t>::max();
  yomuka::sync::failedRecovery = legacyPath();
  assert(migrate() == Result::Failed && !Storage.files.count(destination));

  reset();
  Storage.files[destination] = "existing progress";
  Storage.files[legacyPath()] = legacyRecord(1, 6, false, 42);
  assert(migrate() == Result::Unchanged && Storage.files.at(destination) == "existing progress");

  std::cout << "PASS: TXT legacy resume migration, finished state, corruption, retry, and overwrite protection\n";
}
