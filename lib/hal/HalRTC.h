#pragma once

#include <Rtc.h>

#include <ctime>

class HalRTC {
  bool _available = false;
  mutable Rtc _rtc;

 public:
  // BoardConfigで定義されたRTCを初期化する。未搭載端末ではfalseを返す。
  bool begin();

  // begin() でRTCが検出されたか
  bool isAvailable() const;

  // RTCからUTC時刻を読み取りstruct tmに格納
  bool readTime(struct tm& tm) const;

  // struct tm (UTC) をRTCに書き込み
  bool writeTime(const struct tm& tm) const;
};

extern HalRTC halRTC;
