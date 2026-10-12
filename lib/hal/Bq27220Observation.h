#pragma once

#include <cstdint>

// TI SLUUBD4A 2.27, Table 2-7. Standard-command words are little endian.
// This observer has no bus access and never changes gauge configuration or SOC.
namespace Bq27220Observation {
constexpr uint16_t EDV2 = 0x0008;
constexpr uint16_t VDQ = 0x0010;
constexpr uint16_t INITCOMP = 0x0020;
constexpr uint16_t SMTH = 0x0040;
constexpr uint16_t CFGUPDATE = 0x0400;
constexpr uint16_t SECURITY = 0x0006;
constexpr uint16_t WATCHED = EDV2 | VDQ | INITCOMP | SMTH | CFGUPDATE | SECURITY;

constexpr int flag(uint16_t status, uint16_t mask, bool valid) { return valid ? !!(status & mask) : -1; }

struct Result {
  // Reasons: first observation, EDV2 change, other watched flags, read availability.
  uint8_t reasons = 0;
  int previousStatus = -1;
  uint32_t previousMs = 0;
  uint16_t missed = 0;
};

struct State {
  uint32_t lastStatusMs = 0;
  uint16_t lastStatus = 0;
  uint16_t missed = 0;
  uint8_t lastValid = 0;
  bool seen = false;
  bool haveStatus = false;

  Result observe(uint32_t statusMs, uint16_t status, bool statusOk, bool socOk) {
    Result result;
    result.previousStatus = haveStatus ? lastStatus : -1;
    result.previousMs = lastStatusMs;
    result.missed = missed;
    const uint8_t valid = (socOk ? 1 : 0) | (statusOk ? 2 : 0);
    if (!seen) result.reasons |= 1;
    if (seen && valid != lastValid) result.reasons |= 8;
    if (statusOk) {
      if (haveStatus) {
        const uint16_t changed = lastStatus ^ status;
        if (changed & EDV2) result.reasons |= 2;
        if (changed & (WATCHED & ~EDV2)) result.reasons |= 4;
      }
      lastStatus = status;
      lastStatusMs = statusMs;
      haveStatus = true;
      missed = 0;
    } else if (missed != UINT16_MAX) {
      ++missed;
    }
    seen = true;
    lastValid = valid;
    return result;
  }
};
}  // namespace Bq27220Observation
