#include "signals.h"

#include "config.h"
#include "j1587.h"
#include "settings.h"
#include "uptime.h"

const SigDef kSig[kNumSig] = {
    {"v1437", "Battery +24 V via C66 (pin 1, 1437)", "V", 2, CH_V1437},
    {"v1139", "Starter motor B+, live only while cranking (pin 11, 1139)", "V", 2, CH_V1139},
    {"v1138", "Starter - / shunt top (pin 7, 1138)", "V", 2, CH_V1138},
    {"v1431", "Crank command, key in START (pin 12, 1431)", "V", 2, CH_V1431},
    {"v1274", "Alternator B+ (pin 13, 1274)", "V", 2, CH_V1274},
    {"v2010", "Alternator field, averaged (pin 14, 2010)", "V", 2, CH_V2010},
    {"v1435a", "Alternator ground sense (pin 8, 1435A)", "V", 2, CH_V1435A},
    {"v1435b", "Shunt-end ground (pin 4, 1435B)", "V", 2, CH_V1435B},
    {"v2085", "Diag branch C60-E (pin 16, 2085)", "V", 2, CH_V2085},
    {"shunt", "Battery current, + = discharge (shunt, pins 5/6)", "A", 0, CH_SHUNT_I},
    {"sens5v", "Sensor supply (pin 10, 2000)", "V", 3, CH_SENS5V},
    {"c59_psia", "Fuel inlet pressure C59 (absolute)", "psia", 2, CH_S_C59},
    {"fuel_vac", "Fuel inlet vacuum (baro - C59)", "psi", 2, -1},
    {"oil_in", "Oil filter inlet C62", "psi", 1, CH_S_C62},
    {"oil_out", "Oil filter outlet C61", "psi", 1, CH_S_C61},
    {"oil_dp", "Oil filter pressure drop", "psi", 1, -1},
    {"air", "Air cleaner restriction C63", "psi", 2, CH_S_C63},
    {"c58", "Fuel separator switch C58", "", 0, CH_C58},
};

const char* const kSigStatusText[] = {"ok", "no ADC", "open / unplugged", "sensor supply fault", "out of range"};

static const uint8_t kCurveCh[kNumCurves] = {CH_S_C59, CH_S_C61, CH_S_C62, CH_S_C63};
static const float kSensorOpenV = 0.20f;     // a powered 0.5-4.5 V sensor never reads this low
static const float kSupplyLowV = 4.5f;       // SENS_5V below this = supply fault
static const float kAdcTopV = 3.25f;         // FE outputs near the 3.3 V rail = clipped

float sensorRatio(const AdcSnapshot& a, uint8_t ch) {
  const float vs = a.v[ch];
  float v5 = a.v[CH_SENS5V];
  if (isnan(vs)) {
    return NAN;
  }
  if (isnan(v5) || v5 / kFeBGain < kSupplyLowV) {
    v5 = 5.0f * kFeBGain;  // fall back to nominal so a value still shows (status = supply fault)
  }
  return vs / v5;
}

float sensorUncorrected(const AdcSnapshot& a, uint8_t curve) {
  const float r = sensorRatio(a, kCurveCh[curve]);
  const SensorCurve& c = g_cal.curve[curve];
  if (isnan(r) || c.rHi == c.rLo) {
    return NAN;
  }
  return c.pLo + (r - c.rLo) / (c.rHi - c.rLo) * (c.pHi - c.pLo);
}

float voltageUncorrected(const AdcSnapshot& a, uint8_t sig) {
  const float v = a.v[kSig[sig].adcCh];
  return isnan(v) ? NAN : (v - kFeAOffset) / kFeAGain;
}

static SigValue sensorValue(const AdcSnapshot& a, uint8_t curve, bool applyZero) {
  const uint8_t ch = kCurveCh[curve];
  if (isnan(a.v[ch])) {
    return {NAN, ST_NO_ADC};
  }
  float p = sensorUncorrected(a, curve);
  if (applyZero) {
    p -= g_cal.zero[curve];
  }
  SigStatus st = ST_OK;
  if (a.v[ch] / kFeBGain < kSensorOpenV) {
    st = ST_OPEN;
  } else if (isnan(a.v[CH_SENS5V]) || a.v[CH_SENS5V] / kFeBGain < kSupplyLowV) {
    st = ST_SUPPLY;
  } else if (a.v[ch] > kAdcTopV) {
    st = ST_RANGE;
  }
  return {p, st};
}

static SigStatus worst(SigStatus x, SigStatus y) {
  return x != ST_OK ? x : y;
}

void signalsCompute(const AdcSnapshot& a, SigValue s[kNumSig]) {
  for (uint8_t i = S_V1437; i <= S_V2085; i++) {
    const float raw = a.v[kSig[i].adcCh];
    if (isnan(raw)) {
      s[i] = {NAN, ST_NO_ADC};
      continue;
    }
    s[i] = {voltageUncorrected(a, i) * g_cal.vgain[i], (raw > kAdcTopV || raw < 0.02f) ? ST_RANGE : ST_OK};
  }

  const float vi = a.v[CH_SHUNT_I];
  if (isnan(vi)) {
    s[S_SHUNT] = {NAN, ST_NO_ADC};
  } else {
    s[S_SHUNT] = {(vi - g_cal.shuntZeroV) / kShuntVPerA * g_cal.shuntSign,
                  (vi > kAdcTopV || vi < 0.02f) ? ST_RANGE : ST_OK};
  }

  const float v5 = a.v[CH_SENS5V];
  if (isnan(v5)) {
    s[S_SENS5V] = {NAN, ST_NO_ADC};
  } else {
    s[S_SENS5V] = {v5 / kFeBGain, v5 / kFeBGain < kSupplyLowV ? ST_SUPPLY : ST_OK};
  }

  s[S_C59_PSIA] = sensorValue(a, CURVE_C59, false);
  s[S_FUEL_VAC] = {g_cal.baroPsia - s[S_C59_PSIA].v, s[S_C59_PSIA].st};
  s[S_OIL_IN] = sensorValue(a, CURVE_C62, true);
  s[S_OIL_OUT] = sensorValue(a, CURVE_C61, true);
  s[S_OIL_DP] = {s[S_OIL_IN].v - s[S_OIL_OUT].v, worst(s[S_OIL_IN].st, s[S_OIL_OUT].st)};
  s[S_AIR] = sensorValue(a, CURVE_C63, true);

  const float vc = a.v[CH_C58];
  if (isnan(vc)) {
    s[S_C58] = {NAN, ST_NO_ADC};
  } else {
    s[S_C58] = {vc > kC58OpenV ? 1.0f : 0.0f, ST_OK};  // 0 = closed (normal), 1 = open (restricted or broken wire)
  }
}

// ------------------------------------------------------------------ min/max

static SigMinMax g_mm[kNumSig];
static volatile bool g_mmReset = true;
static portMUX_TYPE g_mmMux = portMUX_INITIALIZER_UNLOCKED;

// Every 20 ms, faster than the dashboard polls, so short peaks such as cranking current are caught.
static void minMaxTask(void*) {
  AdcSnapshot a;
  SigValue s[kNumSig];
  for (;;) {
    adcGet(a);
    signalsCompute(a, s);
    const uint32_t t = uptimeSeconds();
    portENTER_CRITICAL(&g_mmMux);
    const bool reset = g_mmReset;
    g_mmReset = false;
    for (uint8_t i = 0; i < kNumSig; i++) {
      SigMinMax& m = g_mm[i];
      if (reset) {
        m.vmin = m.vmax = NAN;
      }
      const float v = s[i].v;
      if (s[i].st != ST_OK || isnan(v)) {
        continue;
      }
      if (isnan(m.vmin) || v < m.vmin) {
        m.vmin = v;
        m.tmin = t;
      }
      if (isnan(m.vmax) || v > m.vmax) {
        m.vmax = v;
        m.tmax = t;
      }
    }
    portEXIT_CRITICAL(&g_mmMux);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void sigMinMaxBegin() {
  xTaskCreatePinnedToCore(minMaxTask, "minmax", 4096, nullptr, 2, nullptr, 1);
}

void sigMinMaxGet(SigMinMax out[kNumSig]) {
  portENTER_CRITICAL(&g_mmMux);
  memcpy(out, g_mm, sizeof(g_mm));
  portEXIT_CRITICAL(&g_mmMux);
}

void sigMinMaxReset() {
  g_mmReset = true;
}

// Engine state comes only from engine speed (MID 128, PID 190) on J1587.
EngineInfo engineInfo(const SigValue s[kNumSig]) {
  (void)s;
  EngineInfo e = {ENG_UNKNOWN, false, NAN, "no engine speed on J1587 in the last 3 s"};
  float rpm;
  if (j1587Value(128, 190, rpm, 3000)) {
    e.rpmSeen = true;
    e.rpm = rpm;
    e.state = rpm > kRunningRpm ? ENG_RUNNING : ENG_OFF;
    e.reason = "engine speed on J1587";
  }
  return e;
}

// ------------------------------------------------------------------ calibration

static void current(AdcSnapshot& a, SigValue s[kNumSig]) {
  adcGet(a);
  signalsCompute(a, s);
}

static String zeroOne(const String& what, const AdcSnapshot& a, uint32_t epoch) {
  if (what == "shunt") {
    const float v = a.v[CH_SHUNT_I];
    if (isnan(v)) return "shunt: no ADC reading";
    if (fabsf(v - kShuntIdleV) > 0.10f) return "shunt: idle reading is more than 100 A from nominal";
    g_cal.shuntZeroV = v;
    g_cal.calEpoch[kEpochShunt] = epoch;
    return "";
  }
  if (what == "c59") {
    const float p = sensorUncorrected(a, CURVE_C59);
    if (isnan(p)) return "c59: no ADC reading";
    if (p < 10.0f || p > 16.0f) {
      return "c59: reads " + String(p, 2) + " psia; engine-off should be barometric (10-16 psia). Check the sensor curve";
    }
    g_cal.baroPsia = p;
    g_cal.calEpoch[kEpochBaro] = epoch;
    return "";
  }
  int curve = -1;
  if (what == "oil_in") curve = CURVE_C62;
  if (what == "oil_out") curve = CURVE_C61;
  if (what == "air") curve = CURVE_C63;
  if (curve < 0) return what + ": unknown item";
  const float p = sensorUncorrected(a, curve);
  const SensorCurve& c = g_cal.curve[curve];
  if (isnan(p)) return what + ": no ADC reading";
  if (a.v[kCurveCh[curve]] / kFeBGain < kSensorOpenV) return what + ": sensor reads open / unplugged";
  const float limit = 0.15f * fabsf(c.pHi - c.pLo);
  if (fabsf(p - c.pLo) > limit) {
    return what + ": reads " + String(p, 2) + " engine-off, more than 15% of span from zero (absolute sensor or wrong curve?)";
  }
  g_cal.zero[curve] = p - c.pLo;
  g_cal.calEpoch[kEpochCurve0 + curve] = epoch;
  return "";
}

String calZero(const char* what, uint32_t epoch) {
  AdcSnapshot a;
  SigValue s[kNumSig];
  current(a, s);
  calBeginChange();
  const String w(what);
  if (w == "all") {
    const char* items[] = {"shunt", "c59", "oil_in", "oil_out", "air"};
    String errors;
    for (const char* it : items) {
      const String e = zeroOne(it, a, epoch);
      if (e.length()) {
        errors += (errors.length() ? "; " : "") + e;
      }
    }
    saveCal();
    return errors;  // the items that passed are saved even if others refused
  }
  const String err = zeroOne(w, a, epoch);
  if (err.length() == 0) {
    saveCal();
  }
  return err;
}

String calMeter(const char* id, float meterVolts, uint32_t epoch) {
  for (uint8_t i = S_V1437; i <= S_V2085; i++) {
    if (strcmp(kSig[i].id, id) != 0) {
      continue;
    }
    AdcSnapshot a;
    adcGet(a);
    const float raw = voltageUncorrected(a, i);
    if (isnan(raw)) return "no ADC reading";
    if (fabsf(raw) < 5.0f) return "needs at least 5 V on the input to trim the gain";
    const float gain = meterVolts / raw;
    if (gain < 0.9f || gain > 1.1f) return "meter and board differ by more than 10%: check the wiring first";
    calBeginChange();
    g_cal.vgain[i] = gain;
    g_cal.calEpoch[i] = epoch;
    saveCal();
    return "";
  }
  return "unknown voltage id";
}

String calCurve(uint8_t curve, float rLo, float rHi, float pLo, float pHi, uint32_t epoch) {
  if (curve >= kNumCurves) return "unknown curve";
  if (!(rHi > rLo) || rLo < 0.0f || rHi > 1.0f) return "ratios must be 0-1 with high > low";
  if (pHi == pLo) return "pressure span is zero";
  calBeginChange();
  g_cal.curve[curve] = {rLo, rHi, pLo, pHi};
  g_cal.calEpoch[kEpochCurve0 + curve] = epoch;
  saveCal();
  return "";
}

String calReset(const char* what) {
  CalData d;
  calDefaults(d);
  const String w(what);
  calBeginChange();
  if (w == "all") {
    g_cal = d;
  } else if (w == "shunt") {
    g_cal.shuntZeroV = d.shuntZeroV;
    g_cal.shuntSign = 1;
  } else if (w == "c59") {
    g_cal.baroPsia = d.baroPsia;
    g_cal.curve[CURVE_C59] = d.curve[CURVE_C59];
  } else if (w == "oil_in" || w == "oil_out" || w == "air") {
    const uint8_t c = w == "oil_in" ? CURVE_C62 : (w == "oil_out" ? CURVE_C61 : CURVE_C63);
    g_cal.zero[c] = 0;
    g_cal.curve[c] = d.curve[c];
  } else {
    bool found = false;
    for (uint8_t i = S_V1437; i <= S_V2085; i++) {
      if (w == kSig[i].id) {
        g_cal.vgain[i] = 1.0f;
        found = true;
      }
    }
    if (!found) return "unknown item";
  }
  saveCal();
  return "";
}

String calShuntSign(int sign) {
  if (sign != 1 && sign != -1) return "sign must be 1 or -1";
  calBeginChange();
  g_cal.shuntSign = sign;
  saveCal();
  return "";
}
