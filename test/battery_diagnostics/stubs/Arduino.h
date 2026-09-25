#pragma once
#include <cstdint>
inline unsigned long testMillis = 100;
inline unsigned long millis() { return testMillis; }
inline void delay(unsigned long ms) { testMillis += ms; }
inline int getCpuFrequencyMhz() { return 160; }
inline bool setCpuFrequencyMhz(int) { return true; }
constexpr int INPUT = 0;
inline void pinMode(int, int) {}
