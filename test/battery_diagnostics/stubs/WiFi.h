#pragma once
constexpr int WIFI_MODE_NULL = 0;
inline struct {
  int getMode() { return WIFI_MODE_NULL; }
} WiFi;
