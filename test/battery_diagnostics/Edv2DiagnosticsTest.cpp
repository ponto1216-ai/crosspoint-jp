#include <Bq27220Observation.h>
#include <HalPowerManager.h>

#include <algorithm>
#include <iostream>

HalGPIO gpio;
void HalGPIO::begin() { _deviceType = DeviceType::X3; }
void HalGPIO::update() {}
bool HalGPIO::isPressed(uint8_t) const { return false; }

bool contains(const char* text) {
  return std::any_of(logs.begin(), logs.end(),
                     [text](const auto& line) { return line.find(text) != std::string::npos; });
}

int main() {
  using namespace Bq27220Observation;
  static_assert(flag(0x00ae, EDV2, true) == 1 && flag(0x00ae, VDQ, true) == 0);
  static_assert(flag(0x00ae, INITCOMP, true) == 1 && flag(0x00ae, SMTH, true) == 0);
  static_assert(flag(0x00ae, CFGUPDATE, true) == 0 && (0x00ae & SECURITY) == 6);
  static_assert(flag(0xffff, EDV2, false) == -1);
  for (const auto mask : {EDV2, VDQ, INITCOMP, SMTH, CFGUPDATE}) {
    assert(flag(mask, mask, true) == 1 && flag(0, mask, true) == 0);
  }
  State observer;
  auto event = observer.observe(0xfffffff0u, 0x26, true, true);
  assert(event.reasons == 1 && event.previousStatus == -1);
  event = observer.observe(10, 0x2e, true, true);
  assert(event.reasons == 2 && uint32_t(10 - event.previousMs) == 26);
  assert(observer.observe(11, 0x2e, true, true).reasons == 0);
  assert(observer.observe(12, 0, false, true).reasons == 8);
  assert(observer.observe(13, 0, false, true).reasons == 0);
  event = observer.observe(14, 0x26, true, true);
  assert(event.reasons == 10 && event.missed == 2 && event.previousMs == 11);
  assert(observer.observe(15, 0x06, true, true).reasons == 4);  // initialization lost
  assert(observer.observe(16, 0x26, true, true).reasons == 4);
  State unknown;
  assert(unknown.observe(1, 0, false, false).reasons == 1);
  event = unknown.observe(2, 0xae, true, true);
  assert(event.reasons == 8 && event.previousStatus == -1);  // not a synthetic 0 -> 1

  HalPowerManager adc;
  adc.begin();
  adc.getBatteryPercentage();
  assert(Wire.reads.empty() && logs.empty());
#if FREEINK_MCU_C3
  gpio.begin();
  HalPowerManager pm;
  pm.begin();
  Wire.values[0x3a] = 0x26;
  Wire.values[0x2c] = 30;
  Wire.values[0x3c] = 650;
  Wire.values[0x12] = 522;
  Wire.values[0x10] = 36;
  assert(pm.getBatteryPercentage() == 30);
  assert(logs.size() == 3 && contains("edv2=-1->0") && contains("reasons=0x1"));
  const auto firstMs = testMillis;
  const auto poll = [&](uint16_t soc, uint16_t op) {
    logs.clear();
    Wire.reads.clear();
    testMillis += 1500;
    Wire.values[0x2c] = soc;
    Wire.values[0x3a] = op;
    return pm.getBatteryPercentage();
  };
  assert(poll(7, 0xae) == 7);
  assert(logs.size() == 3 && contains("edv2=0->1") && contains("reasons=0x12") && contains("SOC_JUMP"));
  assert(contains(("old_soc_ms=" + std::to_string(firstMs)).c_str()));
  assert(Wire.reads.size() == 8);  // SOC/op once each, six details, never a second snapshot
  const auto reads = Wire.reads;
  pm.getBatteryPercentage();
  assert(Wire.reads == reads && logs.size() == 3);  // cache hit emits nothing
  assert(poll(7, 0xae) == 7);
#if LOG_LEVEL < 2
  assert(logs.empty() && Wire.reads.size() == 2);
#endif
  assert(poll(30, 0xae) == 30 && contains("edv2=1->1") && contains("reasons=0x10"));
  assert(poll(30, 0xa6) == 30 && contains("edv2=1->0") && !contains("SOC_JUMP"));

  Wire.failReg = 0x3a;
  assert(poll(30, 0xae) == 30 && contains("edv2=0->-1") && contains("operation_status=unknown"));
  assert(poll(30, 0xae) == 30);
#if LOG_LEVEL < 2
  assert(logs.empty());
#endif
  Wire.failReg = -1;
  assert(poll(30, 0xae) == 30 && contains("edv2=0->1") && contains("missed=2") && contains("reasons=0xA"));

  Wire.shortReg = 0x2c;
  assert(poll(80, 0xa6) == 30 && contains("soc_ok=0") && contains("soc=-1") && !contains("SOC_JUMP"));
  Wire.shortReg = -1;
  assert(poll(7, 0xa6) == 7 && contains("old_soc=30 soc=7") && contains("reasons=0x18"));
  Wire.shortReg = 0x3a;
  assert(poll(7, 0xae) == 7 && contains("operation_status=unknown"));
  Wire.shortReg = -1;
  assert(poll(7, 0xae) == 7 && contains("missed=1") && contains("edv2=0->1"));

  for (const int reg : {0x08, 0x0c, 0x10, 0x12, 0x3c, 0x0a}) {
    Wire.shortReg = reg;
    const uint16_t next = Wire.values[0x3a] ^ EDV2;
    assert(poll(7, next) == 7 && contains("reasons=0x2"));
    if (reg == 0x0a) assert(contains("battery_status=unknown"));
    if (reg == 0x3c) assert(contains("design_mAh=-1"));
    if (reg == 0x0c) assert(contains("current_mA=-32769"));
  }
  Wire.shortReg = -1;
  // A manual report never consumes an observed transition or modifies the display cache.
  Wire.values[0x3a] ^= EDV2;
  const auto saved = Wire.values;
  const auto snapshot = pm.readGaugeDiagnostics();
  assert(snapshot.valid == 0xff && snapshot.operationStatus == saved.at(0x3a));
  assert(poll(7, saved.at(0x3a)) == 7 && contains("reasons=0x2"));
  assert(Wire.values == saved);
  // Every line, including the worst-case prefix, fits the production 256-byte ring slot.
  for (const auto& line : logs) assert(line.size() + 27 < 256);
  HalPowerManager unavailable;
  unavailable.begin();
  logs.clear();
  Wire.failAll = true;
  assert(unavailable.getBatteryPercentage() == 0 && contains("soc=-1") && contains("edv2=-1->-1"));
  testMillis += 1500;
  logs.clear();
  unavailable.getBatteryPercentage();
#if LOG_LEVEL < 2
  assert(logs.empty());
#endif
  Wire.failAll = false;
  testMillis += 1500;
  logs.clear();
  Wire.readDelayMs = 1;
  const auto startMs = testMillis;
  assert(unavailable.getBatteryPercentage() == 7 && contains("reasons=0x8") && contains("read_ms=8"));
  assert(contains(("op_ms=" + std::to_string(startMs + 2)).c_str()));
  assert(contains("old_soc=-1 soc=7"));
  Wire.readDelayMs = 0;
#endif
  std::cout << "PASS: TI masks, initialization, bidirectional edges, gaps, SOC failures, no duplicate events, ADC "
               "isolation\n";
}
