#include "HalRTC.h"

#include <Logging.h>

HalRTC halRTC;

bool HalRTC::begin() {
  _available = _rtc.begin();
  LOG_DBG("RTC", "%s", _available ? "RTC detected" : "RTC unavailable");
  return _available;
}

bool HalRTC::isAvailable() const { return _available; }

bool HalRTC::readTime(struct tm& tm) const {
  if (!_available) return false;

  Rtc::DateTime dateTime;
  if (!_rtc.now(dateTime)) {
    LOG_DBG("RTC", "RTC read failed");
    return false;
  }

  tm = {};
  tm.tm_sec = dateTime.second;
  tm.tm_min = dateTime.minute;
  tm.tm_hour = dateTime.hour;
  tm.tm_mday = dateTime.day;
  tm.tm_mon = dateTime.month - 1;
  tm.tm_year = dateTime.year - 1900;
  tm.tm_wday = dateTime.weekday;
  tm.tm_isdst = 0;
  if (tm.tm_year < 124 || tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31) {
    LOG_ERR("RTC", "RTC time invalid: %d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return false;
  }
  return true;
}

bool HalRTC::writeTime(const struct tm& tm) const {
  if (!_available) return false;
  const Rtc::DateTime dateTime = {
      static_cast<uint16_t>(tm.tm_year + 1900), static_cast<uint8_t>(tm.tm_mon + 1),
      static_cast<uint8_t>(tm.tm_mday),         static_cast<uint8_t>(tm.tm_hour),
      static_cast<uint8_t>(tm.tm_min),          static_cast<uint8_t>(tm.tm_sec),
      static_cast<uint8_t>(tm.tm_wday),
  };
  if (!_rtc.set(dateTime)) {
    LOG_ERR("RTC", "RTC write failed");
    return false;
  }
  return true;
}
