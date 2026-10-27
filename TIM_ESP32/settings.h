// Settings and calibration kept in flash (Preferences namespace "tim"). They survive firmware updates.
#pragma once

#include <Arduino.h>

// Sensor transfer curve: ratio r = sensor volts / sensor-supply volts, linear from (rLo, pLo) to (rHi, pHi).
struct SensorCurve {
  float rLo, rHi;
  float pLo, pHi;
};

enum CurveIdx : uint8_t { CURVE_C59, CURVE_C61, CURVE_C62, CURVE_C63, kNumCurves };
static const uint8_t kNumVGain = 9;  // order of the voltage signals in signals.h (S_V1437 .. S_V2085)

struct CalData {
  uint16_t version;
  float vgain[kNumVGain];      // meter trim, 1.0 = uncalibrated
  float shuntZeroV;            // SHUNT_I volts at 0 A
  int8_t shuntSign;            // +1, or -1 if the shunt sense wires are reversed
  float zero[kNumCurves];      // engine-off zero in psi (C61, C62, C63; C59 uses baroPsia)
  float baroPsia;              // engine-off C59 reading = barometric pressure
  SensorCurve curve[kNumCurves];
  uint32_t calEpoch[kNumVGain + 2 + kNumCurves];  // when each item was last set (browser clock, 0 = never)
};
// calEpoch index: 0-8 vgain, 9 shunt zero, 10 baro, 11+i curve/zero i
static const uint8_t kEpochShunt = kNumVGain;
static const uint8_t kEpochBaro = kNumVGain + 1;
static const uint8_t kEpochCurve0 = kNumVGain + 2;

struct WifiCfg {
  String apSsid;
  String apPass;  // also the password for the Settings, Calibrate and Update actions (user "admin")
  bool staEnable;
  String staSsid;
  String staPass;
};

extern CalData g_cal;
extern CalData g_calPrev;  // value before the last calibration change, for Undo
extern WifiCfg g_wifi;
extern String g_dash;      // comma-separated signal ids shown on the dashboard, in order

void settingsBegin();
void calDefaults(CalData& c);
void calBeginChange();     // call before editing g_cal: keeps the old value for Undo
void saveCal();
bool calUndo();
void saveWifi();
void saveDash();
void factoryReset();       // calibration, Wi-Fi and dashboard back to defaults
