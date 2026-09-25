#include "HalTiltSensor.h"

#include <HalGPIO.h>
#include <Logging.h>
#include <Wire.h>

HalTiltSensor halTiltSensor;

uint8_t HalTiltSensor::detectAddress() const {
  constexpr uint8_t addresses[] = {0x6B, 0x6A};
  for (const uint8_t address : addresses) {
    Wire.beginTransmission(address);
    Wire.write(static_cast<uint8_t>(0x00));
    if (Wire.endTransmission(false) != 0) continue;
    if (Wire.requestFrom(address, static_cast<uint8_t>(1)) == 1 && Wire.read() == 0x05) return address;
  }
  return 0;
}

void HalTiltSensor::begin() {
  if (!gpio.deviceIsX3()) return;
  _available = _imu.begin();
  if (!_available) {
    LOG_ERR("GYR", "FreeInk IMU initialization failed");
    return;
  }
  _i2cAddr = detectAddress();
  _initMs = millis();
  _lastPollMs = _initMs;
  _isAwake = true;
  LOG_INF("GYR", "FreeInk QMI8658 initialized at 0x%02X", _i2cAddr);
  deepSleep();
}

bool HalTiltSensor::wake() {
  if (!_available) return false;
  if (_isAwake) return true;
  if ((millis() - _initMs) < SLEEP_STABILIZE_MS) return false;
  if (!_imu.wake()) {
    LOG_ERR("GYR", "Failed to wake QMI8658");
    return false;
  }
  _isAwake = true;
  _lastPollMs = _lastTiltMs = _wakeMs = millis();
  LOG_INF("GYR", "QMI8658 woke up");
  return true;
}

bool HalTiltSensor::deepSleep() {
  if (!_available) return false;
  if (!_isAwake) return true;
  if ((millis() - _wakeMs) < SLEEP_STABILIZE_MS && _wakeMs != 0) return false;
  if (!_imu.sleep()) {
    LOG_ERR("GYR", "Failed to put QMI8658 to sleep");
    return false;
  }
  _isAwake = false;
  clearPendingEvents();
  _inTilt = false;
  LOG_INF("GYR", "QMI8658 entered sleep mode");
  return true;
}

bool HalTiltSensor::readSample() {
  if (!_isAwake || (millis() - _lastPollMs) < POLL_INTERVAL_MS) return false;
  _lastPollMs = millis();
  if (!_imu.read(_sample)) {
    ++_readErrorCount;
    return false;
  }
  _hasSample = true;
  return true;
}

float HalTiltSensor::mappedAxis(const uint8_t mode, const uint8_t orientation, const Imu::Sample& sample) const {
  switch (orientation) {
    case CrossPointOrientation::PORTRAIT:
      return mode == CrossPointTiltPageTurn::TILT_INVERTED ? -sample.gx : sample.gx;
    case CrossPointOrientation::INVERTED:
      return mode == CrossPointTiltPageTurn::TILT_INVERTED ? sample.gx : -sample.gx;
    case CrossPointOrientation::LANDSCAPE_CW:
      return mode == CrossPointTiltPageTurn::TILT_INVERTED ? sample.gy : -sample.gy;
    case CrossPointOrientation::LANDSCAPE_CCW:
      return mode == CrossPointTiltPageTurn::TILT_INVERTED ? -sample.gy : sample.gy;
    default:
      return sample.gx;
  }
}

void HalTiltSensor::update(const uint8_t mode, const uint8_t orientation, const bool inReader) {
  if (!_available || _diagnosticsActive) return;
  if (mode != CrossPointTiltPageTurn::TILT_OFF && !_isAwake) {
    wake();
    return;
  }
  if (mode == CrossPointTiltPageTurn::TILT_OFF && _isAwake) {
    deepSleep();
    return;
  }
  if (mode == CrossPointTiltPageTurn::TILT_OFF || !inReader) return;
  const unsigned long now = millis();
  if ((now - _wakeMs) < WAKE_STABILIZE_MS || !readSample()) return;
  const float axis = mappedAxis(mode, orientation, _sample);
  if (_inTilt) {
    if (fabsf(axis) < NEUTRAL_RATE_DPS) _inTilt = false;
    return;
  }
  if ((now - _lastTiltMs) < COOLDOWN_MS) return;
  const bool forward = axis > RATE_THRESHOLD_DPS;
  const bool backward = axis < -RATE_THRESHOLD_DPS;
  if (forward || backward) {
    _tiltForwardEvent = forward;
    _tiltBackEvent = backward;
    _hadActivity = true;
    _inTilt = true;
    _lastTiltMs = now;
    LOG_INF("GYR", "%s Trigger=(%.1f) dps", forward ? "Forward" : "Backward", axis);
  }
}

void HalTiltSensor::recordDiagnostic(const float axis, const unsigned long now) {
  if (axis > _rightPeakDps) _rightPeakDps = axis;
  if (-axis > _leftPeakDps) _leftPeakDps = -axis;
  if (fabsf(axis) >= fabsf(_diagnosticDisplayAxisDps) ||
      ((_diagnosticDisplayAxisDps > 0) != (axis > 0) && fabsf(axis) >= NEUTRAL_RATE_DPS)) {
    _diagnosticDisplayAxisDps = axis;
  }
  const float magnitude = fabsf(axis);
  // Use the quiet interval while the first E-Ink screen is being drawn as the
  // stationary baseline. Deceleration after a gesture is not idle noise.
  if ((now - _diagnosticStartMs) <= 1500 && magnitude < NEUTRAL_RATE_DPS && magnitude > _idleNoiseDps) {
    _idleNoiseDps = magnitude;
  }
  // Direction detection answers whether the sensor itself responds. Reaching
  // the much higher upstream page-turn threshold is evaluated separately from
  // the recorded peaks in the diagnostics UI.
  if (!_diagnosticInMotion) {
    if (axis >= NEUTRAL_RATE_DPS) {
      ++_rightCrossings;
      _diagnosticInMotion = true;
      _diagnosticQuietSinceMs = 0;
    } else if (axis <= -NEUTRAL_RATE_DPS) {
      ++_leftCrossings;
      _diagnosticInMotion = true;
      _diagnosticQuietSinceMs = 0;
    }
  } else if (magnitude < NEUTRAL_RATE_DPS) {
    if (_diagnosticQuietSinceMs == 0) _diagnosticQuietSinceMs = now;
    if ((now - _diagnosticQuietSinceMs) >= DIAGNOSTIC_REARM_MS) {
      _diagnosticInMotion = false;
      _diagnosticQuietSinceMs = 0;
    }
  } else {
    _diagnosticQuietSinceMs = 0;
  }
}

bool HalTiltSensor::beginDiagnostics(const uint8_t orientation) {
  if (!_available) return false;
  _diagnosticsActive = true;
  resetDiagnostics();
  if (!wake()) {
    _diagnosticsActive = false;
    return false;
  }
  _diagnosticStartMs = millis();
  updateDiagnostics(orientation);
  return true;
}

void HalTiltSensor::updateDiagnostics(const uint8_t orientation) {
  if (!_diagnosticsActive || !_isAwake || (millis() - _wakeMs) < WAKE_STABILIZE_MS || !readSample()) return;
  const unsigned long now = millis();
  // mappedAxis() uses the reader's forward/backward convention. On X3 that
  // convention is opposite to the physical left/right labels shown here.
  const float axis = -mappedAxis(CrossPointTiltPageTurn::TILT_NORMAL, orientation, _sample);
  recordDiagnostic(axis, now);
}

void HalTiltSensor::endDiagnostics() {
  _diagnosticsActive = false;
  deepSleep();
}

void HalTiltSensor::resetDiagnostics() {
  _hasSample = false;
  _rightPeakDps = _leftPeakDps = _idleNoiseDps = 0;
  _rightCrossings = _leftCrossings = 0;
  _diagnosticStartMs = millis();
  _diagnosticDisplayAxisDps = 0;
  _diagnosticInMotion = false;
  _diagnosticQuietSinceMs = 0;
}

HalTiltSensor::Diagnostics HalTiltSensor::getDiagnostics(const uint8_t orientation) const {
  Diagnostics d;
  d.available = _available;
  d.awake = _isAwake;
  d.hasSample = _hasSample;
  d.i2cAddress = _i2cAddr;
  d.sample = _sample;
  d.currentAxisDps = -mappedAxis(CrossPointTiltPageTurn::TILT_NORMAL, orientation, _sample);
  d.displayAxisDps = _diagnosticDisplayAxisDps;
  d.axisName[1] =
      (orientation == CrossPointOrientation::LANDSCAPE_CW || orientation == CrossPointOrientation::LANDSCAPE_CCW) ? 'Y'
                                                                                                                  : 'X';
  d.triggerThresholdDps = RATE_THRESHOLD_DPS;
  d.neutralThresholdDps = NEUTRAL_RATE_DPS;
  d.rightPeakDps = _rightPeakDps;
  d.leftPeakDps = _leftPeakDps;
  d.idleNoiseDps = _idleNoiseDps;
  d.readErrorCount = _readErrorCount;
  d.rightCrossings = _rightCrossings;
  d.leftCrossings = _leftCrossings;
  return d;
}

bool HalTiltSensor::wasTiltedForward() {
  const bool value = _tiltForwardEvent;
  _tiltForwardEvent = false;
  return value;
}

bool HalTiltSensor::wasTiltedBack() {
  const bool value = _tiltBackEvent;
  _tiltBackEvent = false;
  return value;
}

bool HalTiltSensor::hadActivity() {
  const bool value = _hadActivity;
  _hadActivity = false;
  return value;
}

void HalTiltSensor::clearPendingEvents() {
  _tiltForwardEvent = false;
  _tiltBackEvent = false;
  _hadActivity = false;
}
