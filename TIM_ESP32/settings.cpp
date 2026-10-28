#include "settings.h"

#include <Preferences.h>

#include "config.h"

static const uint16_t kCalVersion = 1;
static const char* const kDefaultDash = "v1437,v1274,shunt,c59_psia,fuel_vac,oil_in,oil_out,oil_dp,air,c58,j128.190,j128.110";

CalData g_cal;
CalData g_calPrev;
WifiCfg g_wifi;
String g_dash;

static Preferences g_prefs;
static bool g_havePrev = false;

void calDefaults(CalData& c) {
  memset(&c, 0, sizeof(c));
  c.version = kCalVersion;
  for (uint8_t i = 0; i < kNumVGain; i++) {
    c.vgain[i] = 1.0f;
  }
  c.shuntZeroV = kShuntIdleV;
  c.shuntSign = 1;
  c.baroPsia = 14.696f;
  // C59: 0-30 psia absolute (FLIS); the 0.5-4.5 V curve is the usual one, not confirmed.
  c.curve[CURVE_C59] = {0.10f, 0.90f, 0.0f, 30.0f};
  // C61 / C62 oil filter (3246334): range unknown, this is a placeholder.
  c.curve[CURVE_C61] = {0.10f, 0.90f, 0.0f, 100.0f};
  c.curve[CURVE_C62] = {0.10f, 0.90f, 0.0f, 100.0f};
  // C63 air cleaner: Sensata 67CP022-0015AFNA2 (label), 0-15 psia absolute, 0.5-4.5 V ratiometric.
  // Restriction = engine-off reference (zero[CURVE_C63], psia) - reading.
  c.curve[CURVE_C63] = {0.10f, 0.90f, 0.0f, 15.0f};
}

void settingsBegin() {
  g_prefs.begin("tim", false);
  calDefaults(g_cal);
  if (g_prefs.getBytesLength("cal") == sizeof(CalData)) {
    CalData stored;
    g_prefs.getBytes("cal", &stored, sizeof(stored));
    if (stored.version == kCalVersion) {
      g_cal = stored;
      // Before 0.1.10 C63 had a 0-1 psi gauge placeholder; it's a 0-15 psia sensor.
      const SensorCurve& c = g_cal.curve[CURVE_C63];
      if (c.rLo == 0.10f && c.rHi == 0.90f && c.pLo == 0.0f && c.pHi == 1.0f) {
        g_cal.curve[CURVE_C63] = {0.10f, 0.90f, 0.0f, 15.0f};
        g_cal.zero[CURVE_C63] = 0;
      }
    }
  }
  g_calPrev = g_cal;
  g_wifi.apSsid = g_prefs.getString("ap_ssid", kDefaultApSsid);
  g_wifi.apPass = g_prefs.getString("ap_pass", kDefaultApPass);
  g_wifi.staEnable = g_prefs.getBool("sta_en", false);
  g_wifi.staSsid = g_prefs.getString("sta_ssid", "");
  g_wifi.staPass = g_prefs.getString("sta_pass", "");
  g_wifi.staCh1Only = g_prefs.getBool("sta_ch1", true);
  g_dash = g_prefs.getString("dash", kDefaultDash);
}

void calBeginChange() {
  g_calPrev = g_cal;
  g_havePrev = true;
}

void saveCal() {
  g_prefs.putBytes("cal", &g_cal, sizeof(g_cal));
}

bool calUndo() {
  if (!g_havePrev) {
    return false;
  }
  const CalData cur = g_cal;
  g_cal = g_calPrev;
  g_calPrev = cur;
  saveCal();
  return true;
}

void saveWifi() {
  g_prefs.putString("ap_ssid", g_wifi.apSsid);
  g_prefs.putString("ap_pass", g_wifi.apPass);
  g_prefs.putBool("sta_en", g_wifi.staEnable);
  g_prefs.putString("sta_ssid", g_wifi.staSsid);
  g_prefs.putString("sta_pass", g_wifi.staPass);
  g_prefs.putBool("sta_ch1", g_wifi.staCh1Only);
}

void saveDash() {
  g_prefs.putString("dash", g_dash);
}

void factoryReset() {
  g_prefs.clear();
  calDefaults(g_cal);
  g_calPrev = g_cal;
  g_havePrev = false;
  g_wifi = {kDefaultApSsid, kDefaultApPass, false, "", "", true};
  g_dash = kDefaultDash;
}
