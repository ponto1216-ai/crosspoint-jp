#pragma once
#include <Arduino.h>
template <typename... Args> inline void stubLog(Args&&...) {}
#define LOG_ERR(...) stubLog(__VA_ARGS__)
#define LOG_DBG(...) stubLog(__VA_ARGS__)
#define LOG_INF(...) stubLog(__VA_ARGS__)
