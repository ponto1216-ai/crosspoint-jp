#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <Wire.h>
#include <freertos/semphr.h>

#include <cassert>

#include "HalGPIO.h"
#if FREEINK_MCU_C3
#include "Bq27220Observation.h"
#endif

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;

  // C3 retains its established X3 fuel-gauge / X4 ADC paths.  S3 uses the
  // BoardConfig-selected backend in the implementation.
  bool _batteryUseI2C = false;
  bool _gaugeCapacityPending = false;
  mutable int _batteryCachedPercent = 0;         // Last read battery percentage (0-100)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds
#if FREEINK_MCU_C3
  mutable bool _batterySocValid = false;
  mutable uint16_t _batteryLastRawSoc = 0;
  mutable unsigned long _batteryLastRawSocMs = 0;
  mutable Bq27220Observation::State _batteryObservation;
#endif

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode

 public:
  struct AbortedSleepInfo {
    bool aborted = false;
    int wakeupCause = 0;
    int wakePinLevel = 0;
  };

 private:
  AbortedSleepInfo abortedSleepInfo;
  bool bootDiagnosticsCaptured = false;

 public:
  static constexpr int LOW_POWER_FREQ = 10;                    // MHz
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 1500;       // ms
  static constexpr unsigned long BATTERY_ADC_POLL_MS = 60000;  // ms
  static constexpr int BATTERY_SOC_JUMP_THRESHOLD = 15;        // Percentage points, either direction

  void begin();

  // Call once after serial initialization. Keep the consumed SDK record in RAM
  // for manual diagnostic reports, without adding NVS or automatic SD writes.
  void captureBootDiagnostics();
  const AbortedSleepInfo& getAbortedSleepInfo() const { return abortedSleepInfo; }

  // Control CPU frequency for power saving
  void setPowerSaving(bool enabled);

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  // useFullPowerOff: true=GPIO13でバッテリー切断（完全電源断）, false=ESP32ディープスリープ（RTC維持）
  void startDeepSleep(HalGPIO& gpio, bool useFullPowerOff = false) const;

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

  struct GaugeDiagnostics {
    bool supported = false;
    uint8_t capacityStep = 0;
    bool capacityPending = false, capacityChanged = false, capacityVerified = false, capacityHadError = false;
    uint8_t valid = 0;  // SOC, voltage, current, remaining, full, design, battery status, operation status.
    uint16_t soc = 0, voltage = 0, current = 0, remaining = 0, full = 0, design = 0;
    uint16_t batteryStatus = 0, operationStatus = 0;
    unsigned long capturedMs = 0, readMs = 0;
  };
  // Caller holds RenderLock to serialize gauge reads with UI rendering.
  // Register selection only: no Control commands, capacity writes, or cache changes.
  GaugeDiagnostics readGaugeDiagnostics() const;
  // Main-loop only under RenderLock. True while correction/closure is pending.
  bool serviceGaugeCapacity();
  bool gaugeCapacityPending() const { return _gaugeCapacityPending; }

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
