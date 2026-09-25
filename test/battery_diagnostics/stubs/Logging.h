#pragma once
#include <cstdio>
#include <string>
#include <vector>
inline std::vector<std::string> logs;
inline void testLog(const char*, const char* text) { logs.emplace_back(text); }
template <class... Args>
void testLog(const char*, const char* fmt, Args... args) {
  char buf[512];
  std::snprintf(buf, sizeof(buf), fmt, args...);
  logs.emplace_back(buf);
}
#define LOG_INF(...) testLog(__VA_ARGS__)
#define LOG_ERR(...) testLog(__VA_ARGS__)
#if LOG_LEVEL >= 2
#define LOG_DBG(...) testLog(__VA_ARGS__)
#else
#define LOG_DBG(...)
#endif
