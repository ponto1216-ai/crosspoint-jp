#!/usr/bin/env python3
"""Check production power transitions against an instrumented nonrecursive mutex.

Extract the actual methods, as in run_render_scheduling_test.py. The clock
stub rejects the old read/check/switch race window. This is a deterministic
critical-section check, not a FreeRTOS or hardware timing simulation.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default=os.environ.get('CXX', 'c++'))
parser.add_argument('--zig', action='store_true')
parser.add_argument('--source', type=Path, default=root / 'lib/hal/HalPowerManager.cpp')
args = parser.parse_args()
production = args.source.read_text(encoding='utf-8')
transition = production[production.index('void HalPowerManager::setPowerSaving('):
                        production.index('void HalPowerManager::startDeepSleep(')]
locks = production[production.index('HalPowerManager::Lock::Lock()'):]
source = r'''
#include <cassert>
#include <cstdio>
struct Mutex { bool held = false; } mutex;
constexpr int portMAX_DELAY = 0;
int takes = 0, gives = 0, calls = 0, frequency = 160;
bool failClock = false;
void xSemaphoreTake(Mutex* m, int) {
  assert(!m->held);  // Recursive acquisition would deadlock on the device.
  m->held = true; ++takes;
}
void xSemaphoreGive(Mutex* m) {
  assert(m->held); m->held = false; ++gives;
}
bool setCpuFrequencyMhz(int mhz) {
  assert(mutex.held);  // A competing Lock must be excluded through the switch.
  ++calls;
  if (failClock) return false;
  frequency = mhz; return true;
}
constexpr int WIFI_MODE_NULL = 0;
struct { int mode = 0; int getMode() {
  assert(!mutex.held); return mode;
} } WiFi;
#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
class HalPowerManager {
 public:
  enum LockMode { None, NormalSpeed };
  int normalFreq = 160;
  bool isLowPower = false;
  LockMode currentLockMode = None;
  Mutex* modeMutex = &mutex;
  static constexpr int LOW_POWER_FREQ = 10;
  void setPowerSaving(bool);
  class Lock {
    bool valid = false;
   public:
    Lock(); ~Lock();
  };
} powerManager;
''' + transition + locks + r'''
void check(int mhz, bool low) {
  assert(frequency == mhz && powerManager.isLowPower == low);
  assert(!mutex.held && takes == gives);
}
int main() {
  auto& pm = powerManager;
  pm.normalFreq = 0;
  pm.setPowerSaving(true);
  assert(takes == 0 && calls == 0);
  pm.normalFreq = 160;
  pm.setPowerSaving(false); check(160, false);  // no change
  failClock = true;
  pm.setPowerSaving(true); check(160, false);   // failed low transition
  failClock = false;
  pm.setPowerSaving(true); check(10, true);
  int previous = calls;
  pm.setPowerSaving(true); check(10, true);
  assert(calls == previous);
  failClock = true;
  pm.setPowerSaving(false); check(10, true);    // failed restore
  failClock = false;
  { // Low-power transition wins first: constructor restores before returning.
    HalPowerManager::Lock lock;
    check(160, false);
    assert(pm.currentLockMode == HalPowerManager::NormalSpeed);
    pm.setPowerSaving(true); check(160, false);
    { HalPowerManager::Lock duplicate; }       // invalid lock must not release owner
    assert(pm.currentLockMode == HalPowerManager::NormalSpeed);
  }
  assert(pm.currentLockMode == HalPowerManager::None);
  { // Lock wins first: idle request cannot lower the clock.
    HalPowerManager::Lock lock;
    pm.setPowerSaving(true); check(160, false);
  }
  for (int mode : {1, 2, 3}) {                // STA, AP, AP+STA
    pm.setPowerSaving(true); check(10, true);
    WiFi.mode = mode;
    pm.setPowerSaving(true); check(160, false);
    pm.setPowerSaving(true); check(160, false);
    WiFi.mode = WIFI_MODE_NULL;
  }
  pm.setPowerSaving(true); check(10, true);
  puts("PASS: power mutex, both lock orders, no-op, failures, Wi-Fi and invalid state");
}
'''
source = '#include <initializer_list>\n' + source
with tempfile.TemporaryDirectory(prefix='yomuka-power-') as directory:
    path = Path(directory)
    cpp = path / 'test.cpp'
    cpp.write_text(source, encoding='utf-8')
    exe = path / 'test.exe'
    command = [args.compiler] + (['c++'] if args.zig else [])
    subprocess.run(command + ['-std=c++17', '-Wall', '-Wextra', '-Werror',
                             str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
