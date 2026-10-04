#!/usr/bin/env python3
"""Run actual recent-list mutations with an allocation spy and save spy.

Use the production header and add/update/move methods. Storage serialization
is replaced by a save spy, so allocation counts cover list updates only.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default=os.environ.get('CXX', 'c++'))
parser.add_argument('--zig', action='store_true')
parser.add_argument('--source', type=Path, default=root / 'src/RecentBooksStore.cpp')
args = parser.parse_args()
production = args.source.read_text(encoding='utf-8')
methods = production[production.index('void RecentBooksStore::addBook('):
                     production.index('bool RecentBooksStore::saveToFile()')]
source = r'''
#include "RecentBooksStore.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <new>
bool measuring = false;
size_t allocations = 0;
int saves = 0;
void* operator new(size_t size) {
  if (measuring) ++allocations;
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
constexpr int MAX_RECENT_BOOKS = 10;
RecentBooksStore RecentBooksStore::instance;
bool RecentBooksStore::saveToFile() const { ++saves; return true; }
''' + methods + r'''
void add(RecentBooksStore& store, const RecentBook& b) {
  store.addBook(b.path, b.title, b.author, b.coverBmpPath, b.bookId);
}
std::array<const char*, 4> buffers(const RecentBook& b) {
  return {b.path.data(), b.title.data(), b.author.data(), b.coverBmpPath.data()};
}
int main() {
  std::array<RecentBook, 12> inputs;
  for (size_t i = 0; i < inputs.size(); ++i) {
    std::string key = std::to_string(i);
    inputs[i] = {"/books/long-directory-name/book-" + key + ".epub",
                 "A long title beyond the small-string buffer " + key,
                 "A long author beyond the small-string buffer " + key,
                 "/.crosspoint/long-cover-cache-path/" + key + ".bmp", i + 1};
  }
  RecentBooksStore store;
  for (int i = 0; i < 10; ++i) add(store, inputs[i]);
  assert(store.getCount() == 10 && saves == 10);
  std::array<std::array<const char*, 4>, 10> original;
  for (const auto& b : store.getBooks()) original[b.bookId - 1] = buffers(b);
  std::array<uint64_t, 10> expected;
  for (size_t i = 0; i < expected.size(); ++i) expected[i] = 10 - i;
  for (int turn = 0; turn < 1000; ++turn) {
    size_t index = turn % 10;  // Includes already-first, middle and last entries.
    const auto id = inputs[index].bookId;
    auto it = std::find(expected.begin(), expected.end(), id);
    std::rotate(expected.begin(), it, it + 1);
    allocations = 0;
    int beforeSaves = saves;
    measuring = true;
    add(store, inputs[index]);
    measuring = false;
    if (allocations != 0) std::fprintf(stderr, "reopen list allocations: %zu\n", allocations);
    assert(allocations == 0 && saves == beforeSaves + 1);
    assert(store.getCount() == 10);
    for (size_t i = 0; i < expected.size(); ++i) {
      const auto& b = store.getBooks()[i];
      assert(b.bookId == expected[i]);
      assert(buffers(b) == original[b.bookId - 1]);
    }
  }
  RecentBook changed = inputs[3];
  auto prior = original[3];
  changed.path = "/renamed-and-moved/different-long-directory/book-3.epub";
  add(store, changed);  // Same BookId, new path: retain one entry and metadata buffers.
  assert(store.getCount() == 10 && store.getBooks().front().path == changed.path);
  assert(store.getBooks().front().bookId == changed.bookId);
  auto after = buffers(store.getBooks().front());
  for (size_t i = 1; i < after.size(); ++i) assert(after[i] == prior[i]);
  changed.bookId = 444;  // Same path, changed archive fingerprint.
  allocations = 0; measuring = true; add(store, changed); measuring = false;
  assert(allocations == 0 && store.getCount() == 10);
  assert(store.getBooks().front().bookId == 444);
  prior = buffers(store.getBooks().front());
  assert(prior == after);
  changed.bookId = 0;  // Preserve legacy/non-EPUB replacement behavior.
  add(store, changed);
  assert(store.getBooks().front().bookId == 0);
  changed.title = std::string(200, 't');
  changed.author = std::string(210, 'a');
  changed.coverBmpPath = std::string(220, 'c');
  add(store, changed);
  const auto& metadata = store.getBooks().front();
  assert(metadata.title == changed.title && metadata.author == changed.author);
  assert(metadata.coverBmpPath == changed.coverBmpPath && metadata.path.data() == prior[0]);
  allocations = 0; measuring = true; add(store, changed); measuring = false;
  assert(allocations == 0);  // Updated metadata is reused on the next open.
  add(store, inputs[10]);
  assert(store.getCount() == 10 && store.getBooks().front().bookId == 11);
  assert(std::none_of(store.getBooks().begin(), store.getBooks().end(),
                     [&](const RecentBook& b) { return b.bookId == expected.back(); }));
  RecentBooksStore legacy;
  RecentBook first = inputs[0], second = inputs[1];
  first.bookId = second.bookId = 0;
  add(legacy, first); add(legacy, second);
  assert(legacy.getCount() == 2);  // Zero BookIds must never match different paths.
  add(legacy, first);
  assert(legacy.getCount() == 2 && legacy.getBooks().front().path == first.path);
  first.bookId = 1234; add(legacy, first);
  assert(legacy.getCount() == 2 && legacy.getBooks().front().bookId == 1234);
  const std::string movedPath = "/archive/renamed-long-book-name.epub";
  int beforeSaves = saves;
  legacy.moveBook(first.path, movedPath);
  assert(saves == beforeSaves + 1 && legacy.getCount() == 2);
  assert(legacy.getBooks().front().path == movedPath && legacy.getBooks().front().bookId == 1234);
  assert(legacy.getBooks().front().coverBmpPath.empty());
  first.path = movedPath; add(legacy, first);
  assert(legacy.getCount() == 2 && legacy.getBooks().front().coverBmpPath == first.coverBmpPath);
  legacy.updateBook(movedPath, changed.title, changed.author, changed.coverBmpPath);
  assert(legacy.getBooks().front().bookId == 1234);
  assert(legacy.getBooks().front().title == changed.title);
  beforeSaves = saves;
  legacy.moveBook("missing", movedPath);
  legacy.updateBook("missing", "", "", "");
  assert(saves == beforeSaves);
  puts("PASS: 1000 allocation-free reopens, ordering, BookId/path changes, metadata, legacy, cap and move/update");
}
'''
with tempfile.TemporaryDirectory(prefix='yomuka-recents-') as directory:
    path = Path(directory)
    cpp = path / 'test.cpp'
    cpp.write_text(source, encoding='utf-8')
    exe = path / 'test.exe'
    command = [args.compiler] + (['c++'] if args.zig else [])
    subprocess.run(command + ['-std=c++17', '-Wall', '-Wextra', '-Werror',
                             '-I' + str(root / 'src'), str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
