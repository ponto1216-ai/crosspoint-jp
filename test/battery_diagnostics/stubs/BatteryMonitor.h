#pragma once
#include <cstdint>
inline int adcPercent = 60, adcReads = 0;
class BatteryMonitor {
 public:
  BatteryMonitor(int = 0) {}
  int readPercentage() const {
    ++adcReads;
    return adcPercent;
  }
  bool readPercentageChecked(uint16_t& value) const {
    value = readPercentage();
    return true;
  }
};
