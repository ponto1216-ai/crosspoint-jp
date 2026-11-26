#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
namespace FsHelpers {
inline bool hasExtension(std::string_view path, std::string_view extension) {
  if (path.size() < extension.size()) return false;
  std::string suffix(path.substr(path.size() - extension.size()));
  std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return std::tolower(c); });
  return suffix == extension;
}
inline bool hasTxtExtension(std::string_view path) { return hasExtension(path, ".txt"); }
inline bool hasMarkdownExtension(std::string_view path) { return hasExtension(path, ".md"); }
inline bool hasBmpExtension(std::string_view path) { return hasExtension(path, ".bmp"); }
inline bool hasJpgExtension(std::string_view path) { return hasExtension(path, ".jpg") || hasExtension(path, ".jpeg"); }
inline std::string normalisePath(const std::string& path) { return path; }
}
