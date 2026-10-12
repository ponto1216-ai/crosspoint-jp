#pragma once
#include <cassert>
#include <cstdint>
#include <map>
#include <vector>
struct TestWire {
  std::map<uint8_t, uint16_t> values{{0x2c, 84},  {0x08, 3700}, {0x0c, 0xff88}, {0x10, 210},
                                     {0x12, 300}, {0x3c, 3000}, {0x0a, 0x0109}, {0x3a, 0x042e}};
  std::vector<uint8_t> reads;
  int failReg = -1, shortReg = -1, selected = 0, offset = 0, count = 0;
  bool failAll = false;
  unsigned long readDelayMs = 0;
  void begin(int, int, int) {}
  void setTimeOut(int) {}
  void beginTransmission(uint8_t addr) { assert(addr == 0x55); }
  void write(uint8_t reg) {
    selected = reg;
    reads.push_back(reg);
  }
  int endTransmission(bool stop) {
    assert(!stop);
    return failAll || selected == failReg ? 1 : 0;
  }
  void requestFrom(uint8_t addr, uint8_t n) {
    assert(addr == 0x55 && n == 2);
    testMillis += readDelayMs;
    count = selected == shortReg ? 1 : 2;
    offset = 0;
  }
  int available() const { return count - offset; }
  int read() { return (values[selected] >> (8 * offset++)) & 0xff; }
};
inline TestWire Wire;
