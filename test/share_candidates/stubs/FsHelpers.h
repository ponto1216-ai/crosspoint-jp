#pragma once
#include <string>
namespace FsHelpers {
inline bool hasEpubExtension(const std::string& path) {
  if (path.size() < 5) return false;
  auto extension = path.substr(path.size() - 5);
  for (char& c : extension)
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return extension == ".epub";
}
}  // namespace FsHelpers
