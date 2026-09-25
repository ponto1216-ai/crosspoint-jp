#include <HalPowerManager.h>

#include <iostream>
HalGPIO gpio;
void HalGPIO::begin() { _deviceType = DeviceType::X3; }
void HalGPIO::update() {}
bool HalGPIO::isPressed(uint8_t) const { return false; }
int main() {
  // C3 X4 and S3 must never read the X3 I2C fuel gauge.
  HalPowerManager adc;
  adc.begin();
  assert(adc.getBatteryPercentage() == 60 && adcReads == 1 && Wire.reads.empty());
  adcPercent = 80;
  testMillis += 60000;
  assert(adc.getBatteryPercentage() == 62 && Wire.reads.empty());
#if FREEINK_MCU_C3
  gpio.begin();
  HalPowerManager pm;
  pm.begin();
  assert(pm.getBatteryPercentage() == 84);
  const auto count = Wire.reads.size();
  assert(pm.getBatteryPercentage() == 84 && Wire.reads.size() == count);
#if LOG_LEVEL < 2
  assert(count == 1 && logs.empty());  // Initial value is not a jump from synthetic zero.
#else
  assert(count == 5 && logs.back().find("old_soc=-1 soc=84") != std::string::npos);
#endif
  logs.clear();
  Wire.reads.clear();
  testMillis += 1500;
  Wire.values[0x2c] = 7;
  assert(pm.getBatteryPercentage() == 7);
  assert((Wire.reads == std::vector<uint8_t>{0x2c, 0x08, 0x0c, 0x10, 0x12}));
  assert(logs.size() == 1 && logs.back().find("SOC_JUMP") != std::string::npos);
  assert(
      logs.back().find("old_soc=84 soc=7 voltage_mV=3700 current_mA=-120 remaining_mAh=210 full_mAh=300 valid=0xF") !=
      std::string::npos);
  // Failed SOC writes / short reads preserve the last display and last successful raw SOC.
  Wire.failReg = 0x2c;
  testMillis += 1500;
  Wire.values[0x2c] = 55;
  assert(pm.getBatteryPercentage() == 7);
  Wire.failReg = -1;
  Wire.shortReg = 0x2c;
  testMillis += 1500;
  assert(pm.getBatteryPercentage() == 7);
  Wire.shortReg = -1;
  testMillis += 1500;
  assert(pm.getBatteryPercentage() == 55 && logs.back().find("old_soc=7 soc=55") != std::string::npos);
  // Individual diagnostic failures do not poison SOC or the remaining fields.
  for (int reg : {0x08, 0x0c, 0x10, 0x12}) {
    Wire.failReg = reg;
    testMillis += 1500;
    Wire.values[0x2c] = Wire.values[0x2c] == 55 ? 80 : 55;
    assert(pm.getBatteryPercentage() == Wire.values[0x2c]);
    assert(logs.back().find("valid=0xF") == std::string::npos);
    if (reg == 0x0c) assert(logs.back().find("current_mA=-32769") != std::string::npos);
  }
  Wire.failReg = -1;
  // Exactly 15 points logs, 14 does not in release; a genuine zero remains valid.
  testMillis += 1500;
  Wire.values[0x2c] = 0;
  assert(pm.getBatteryPercentage() == 0);
  logs.clear();
  testMillis += 1500;
  Wire.values[0x2c] = 14;
  assert(pm.getBatteryPercentage() == 14);
#if LOG_LEVEL < 2
  assert(logs.empty());
#endif
  testMillis += 1500;
  Wire.values[0x2c] = 29;
  assert(pm.getBatteryPercentage() == 29);
  assert(logs.back().find("SOC_JUMP") != std::string::npos);
  testMillis += 1500;
  Wire.values[0x2c] = 200;
  assert(pm.getBatteryPercentage() == 100);
  assert(logs.back().find("soc=200") != std::string::npos);  // Log raw word; retain existing UI clamp.
#endif
  std::cout << "PASS: battery poll, signed current, jump threshold, failure retention and ADC isolation\n";
}
