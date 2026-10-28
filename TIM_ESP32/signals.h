// Engineering values from the ADC channels, engine state, and engine-off calibration.
#pragma once

#include <Arduino.h>

#include "adc.h"

enum SigId : uint8_t {
  // voltages: same order as CalData::vgain
  S_V1437, S_V1139, S_V1138, S_V1431, S_V1274, S_V2010, S_V1435A, S_V1435B, S_V2085,
  S_SHUNT, S_SENS5V,
  S_C59_PSIA, S_FUEL_VAC, S_OIL_IN, S_OIL_OUT, S_OIL_DP, S_AIR,
  S_C58,
  kNumSig
};

enum SigStatus : uint8_t { ST_OK, ST_NO_ADC, ST_OPEN, ST_SUPPLY, ST_RANGE };

struct SigDef {
  const char* id;
  const char* name;
  const char* unit;
  uint8_t decimals;
  int8_t adcCh;  // -1 = computed from other signals
};

struct SigValue {
  float v;
  SigStatus st;
};

enum EngineState : uint8_t { ENG_OFF, ENG_CRANKING, ENG_RUNNING, ENG_UNKNOWN };

struct EngineInfo {
  EngineState state;
  bool rpmSeen;     // engine speed heard on J1587 in the last 3 s
  float rpm;
  const char* reason;
};

// Dashboard min/max of each engineering value since boot or the last clear (RAM only).
struct SigMinMax {
  float vmin, vmax;     // NAN = no good reading yet
  uint32_t tmin, tmax;  // uptime second each was reached
};

extern const SigDef kSig[kNumSig];
extern const char* const kSigStatusText[];

void signalsCompute(const AdcSnapshot& a, SigValue out[kNumSig]);
EngineInfo engineInfo(const SigValue s[kNumSig]);
void sigMinMaxBegin();                       // starts the 50 Hz tracking task
void sigMinMaxGet(SigMinMax out[kNumSig]);
void sigMinMaxReset();
float sensorRatio(const AdcSnapshot& a, uint8_t ch);   // NAN if no data
float sensorUncorrected(const AdcSnapshot& a, uint8_t curve);  // curve output before the zero offset
float voltageUncorrected(const AdcSnapshot& a, uint8_t sig);   // FE-A input volts before the meter trim

// Calibration. Each returns "" on success or a reason it refused. epoch = browser clock (0 if unknown).
String calZero(const char* what, uint32_t epoch);  // "all", "shunt", "c59" (baro), "oil_in", "oil_out", "air"
String calMeter(const char* id, float meterVolts, uint32_t epoch);
String calCurve(uint8_t curve, float rLo, float rHi, float pLo, float pHi, uint32_t epoch);
String calReset(const char* what);  // one item id, or "all"
String calShuntSign(int sign);      // -1 if cranking current reads negative (sense wires reversed)
