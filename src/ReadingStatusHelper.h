#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class ReadingStatus : uint8_t {
  Unread,   // progress.bin が存在しない
  Reading,  // progress.bin が存在し、読了フラグなし
  Finished  // progress.bin が存在し、読了フラグあり
};

// The browser keeps the expensive per-book SD-card checks in this compact
// on-card index. Unknown values deliberately fall back to the authoritative
// per-book files, so a missing or corrupt index can only cost time, never show
// an incorrect status.
enum class CachedBookStatus : uint8_t { NotGenerated, Resumable, Complete, Unknown = 0xff };

struct BookListStatusEntry {
  std::string cacheEntryName;
  ReadingStatus readingStatus = ReadingStatus::Unread;
  CachedBookStatus cacheStatus = CachedBookStatus::Unknown;
};

void loadBookListStatusIndex(const std::string& cacheDir, std::vector<BookListStatusEntry>& entries);
bool getBookListStatusFromIndex(const std::string& filepath, const std::vector<std::string>& cacheEntries,
                                const std::vector<BookListStatusEntry>& entries, ReadingStatus& readingStatus,
                                CachedBookStatus& cacheStatus);
void updateBookListStatusIndex(const std::string& filepath, ReadingStatus readingStatus, CachedBookStatus cacheStatus,
                               std::vector<BookListStatusEntry>& entries);
bool saveBookListStatusIndex(const std::string& cacheDir, const std::vector<BookListStatusEntry>& entries);
void removeBookListStatusIndexEntry(const std::string& filepath, std::vector<BookListStatusEntry>& entries);
void invalidateBookListStatusIndexEntry(const std::string& filepath, const std::string& cacheDir);

// ファイルパスからSDカード上のキャッシュを確認し、読書状態を返す。
// filepath: 書籍ファイルの絶対パス（例: "/books/sample.epub"）
// cacheDir: キャッシュルート（通常 "/.crosspoint"）
ReadingStatus getReadingStatus(const std::string& filepath, const std::string& cacheDir);

// Build a sorted index of book-cache directory names once. Call
// getReadingStatusFromCacheEntries() for visible files to avoid reading every
// progress.bin while loading a large directory.
void getReadingStatusCacheEntries(const std::string& cacheDir, std::vector<std::string>& cacheEntries);
bool hasBookCacheEntry(const std::string& filepath, const std::vector<std::string>& cacheEntries);
bool getReadingStatusFromCacheEntries(const std::string& filepath, const std::string& cacheDir,
                                      const std::vector<std::string>& cacheEntries, ReadingStatus& status);

// Read statuses for a directory listing with one cache-directory scan. Files
// without a matching cache entry remain Unread, avoiding one failed SD open
// for every unread book.
void getReadingStatuses(const std::string& basePath, const std::vector<std::string>& filenames,
                        const std::string& cacheDir, std::vector<ReadingStatus>& statuses);

// 指定ファイルを既読（isFinished=1）にマークする。
// progress.bin が存在する場合は末尾バイトのみ更新して読書位置を保持。
// 存在しない場合は新規作成（spineIndex/page等はゼロ初期化）。
// EPUB/XTC 形式を拡張子から自動判定。
// 戻り値: 成功時 true、EPUB/XTC以外やファイルI/O失敗時 false。
bool markAsFinished(const std::string& filepath, const std::string& cacheDir);
