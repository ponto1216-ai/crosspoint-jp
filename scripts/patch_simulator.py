"""Apply Yomuka-only API shims to the downloaded desktop simulator library."""

from pathlib import Path
Import("env")

library = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV") / "simulator" / "src"


def replace_once(name, old, new):
    path = library / name
    contents = path.read_text(encoding="utf-8")
    if new in contents:
        return
    if contents.count(old) != 1:
        raise RuntimeError(f"Unexpected simulator version in {path}; cannot apply Yomuka shim")
    path.write_text(contents.replace(old, new), encoding="utf-8")


replace_once("HalStorage.h", "class StorageLock; // private class, used internally",
             "class StorageLock { public: StorageLock() = default; ~StorageLock() = default; }; // host SD is synchronous")
replace_once("HalGPIO.h", "  bool wasAnyPressed() const;",
             "  bool wasAnyPressed() const;\n  bool rawInputActive() { return wasAnyPressed(); }")
replace_once("HalPowerManager.h", "  uint16_t getBatteryPercentage() const;",
             """  uint16_t getBatteryPercentage() const;
  struct GaugeDiagnostics {
    bool supported = false;
    uint8_t capacityStep = 0;
    bool capacityPending = false, capacityChanged = false, capacityVerified = false, capacityHadError = false;
    uint8_t valid = 0;
    uint16_t soc = 0, voltage = 0, current = 0, remaining = 0, full = 0, design = 0;
    uint16_t batteryStatus = 0, operationStatus = 0;
    unsigned long capturedMs = 0, readMs = 0;
  };
  GaugeDiagnostics readGaugeDiagnostics() const { return {}; }""")
replace_once("HalPowerManager.h", "  GaugeDiagnostics readGaugeDiagnostics() const { return {}; }",
             "  GaugeDiagnostics readGaugeDiagnostics() const { return {}; }\n  bool gaugeCapacityPending() const { return false; }\n  bool serviceGaugeCapacity() { return false; }")
replace_once("HalStorage.h", "  bool isOpen() const;",
             "  bool isOpen() const;\n  int getError() const { return 0; }")
replace_once("BatteryMonitor.h", "  BatteryMonitor(int pin, int factor = -1) {}",
             "  BatteryMonitor() = default;\n  BatteryMonitor(int pin, int factor = -1) {}")
replace_once("BatteryMonitor.h", "  int getVoltage() { return 4200; }",
             "  int getVoltage() { return 4200; }\n  int readMillivolts() const { return 4200; }\n  static int percentageFromMillivolts(int) { return 100; }")
replace_once("NetworkClient.h", "  virtual int available() { return 0; }",
             "  void setTimeout(uint32_t) {}\n  virtual int available() { return 0; }")
replace_once("NetworkClient.h", "class Stream;", "class Stream;\nclass IPAddress;")
replace_once("NetworkClient.h", "  virtual int connect(const char *host, uint16_t port);",
             "  virtual int connect(const char *host, uint16_t port);\n  int connect(const IPAddress&, uint16_t, int) { return 0; }\n  void setConnectionTimeout(uint32_t) {}")
replace_once("WiFi.h", "  String macAddress() { return String(\"02:00:00:00:00:01\"); }",
             "  String macAddress() { return String(\"02:00:00:00:00:01\"); }\n  String softAPmacAddress() { return macAddress(); }")
replace_once("esp_system.h", "#pragma once", "#pragma once\n#include <cstdint>\n#include <cstdlib>\ninline uint32_t esp_random() { return static_cast<uint32_t>(std::rand()); }")
replace_once("ESPmDNS.h", "  void addService(const char *, const char *, int) {}",
             """  void addService(const char *, const char *, int) {}
  void addServiceTxt(const char *, const char *, const char *, const char *) {}
  int queryService(const char *, const char *) { return 0; }
  IPAddress address(int) { return {}; }
  int port(int) { return 0; }
  String hostname(int) { return {}; }""")
replace_once("ESPmDNS.h", "#pragma once\n", "#pragma once\n#include <WiFi.h>\n")

server = library / "NetworkServer.h"
if not server.exists():
    server.write_text("""#pragma once
#include <WiFi.h>
#include <cstdint>
// Networking is not part of the simulator layout test.
class NetworkServer {
 public:
  NetworkServer(uint16_t, int = 1) {}
  void begin() {}
  void end() {}
  bool hasClient() { return false; }
  NetworkClient accept() { return {}; }
  explicit operator bool() const { return false; }
};
""", encoding="utf-8")
replace_once("NetworkServer.h", "#include <NetworkClient.h>", "#include <WiFi.h>")

preferences = library / "Preferences.h"
if not preferences.exists():
    preferences.write_text("""#pragma once
#include <WString.h>
// Layout simulator: credentials are never persisted or used to connect.
class Preferences {
 public:
  bool begin(const char *, bool) { return false; }
  String getString(const char *, const char *fallback) { return fallback; }
  size_t putString(const char *, const char *) { return 0; }
  void end() {}
};
""", encoding="utf-8")
