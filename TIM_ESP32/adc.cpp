#include "adc.h"

#include <Wire.h>

#include "config.h"

const char* const kAdcChName[kNumAdcCh] = {
    "SHUNT_I", "V_1139", "V_1138", "V_1431", "V_1274", "V_2010", "V_1435A", "V_1437",
    "S_C59", "S_C61", "S_C62", "S_C63", "SENS5V_MON", "C58_SW", "V_2085", "V_1435B"};
const char* const kAdcChPin[kNumAdcCh] = {
    "5,6 / 1829,1828", "11 / 1139", "7 / 1138", "12 / 1431", "13 / 1274", "14 / 2010", "8 / 1435A", "1 / 1437",
    "29 / 2075", "27 / 2065", "23 / 2045", "19 / 2025", "10 / 2000", "21 / 2035", "16 / 2085", "4 / 1435B"};

static const uint8_t kRegConv = 0x00;
static const uint8_t kRegConfig = 0x01;
static const float kFilterAlpha = 0.25f;  // EMA per scan (~60 scans/s)
// The alternator field wire (pin 14) is switched by the regulator (PWM), so single samples jump between
// about 14 and 29 V. A ~1.5 s average shows the mean field voltage instead.
static const float kFilterAlphaField = 0.01f;

static AdcSnapshot g_snap;
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool g_resetMinMax = true;

// Single-shot, AINx vs GND, PGA +/-4.096 V, 475 SPS, comparator off (ALRT high-Z).
static uint16_t configWord(uint8_t ain) {
  return 0x8000 | ((4 + ain) << 12) | (1 << 9) | (1 << 8) | (6 << 5) | 0x0003;
}

static bool writeReg(uint8_t addr, uint8_t reg, uint16_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value >> 8);
  Wire.write(value & 0xFF);
  return Wire.endTransmission() == 0;
}

static bool readReg(uint8_t addr, uint8_t reg, int16_t& value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, (uint8_t)2) != 2) {
    return false;
  }
  value = (int16_t)((Wire.read() << 8) | Wire.read());
  return true;
}

static void adcTask(void*) {
  float v[kNumAdcCh] = {0};
  bool have[kNumAdcCh] = {false};
  uint32_t lastRateMs = millis();
  uint32_t scansAtRate = 0;
  for (;;) {
    bool ok[4] = {true, true, true, true};
    for (uint8_t ain = 0; ain < 4; ain++) {
      bool started[4];
      for (uint8_t a = 0; a < 4; a++) {
        started[a] = ok[a] && writeReg(kAdcAddr[a], kRegConfig, configWord(ain));
        ok[a] = started[a];
      }
      vTaskDelay(pdMS_TO_TICKS(3));  // 475 SPS = 2.1 ms per conversion
      for (uint8_t a = 0; a < 4; a++) {
        if (!started[a]) {
          continue;
        }
        int16_t raw;
        if (!readReg(kAdcAddr[a], kRegConv, raw)) {
          ok[a] = false;
          continue;
        }
        const uint8_t ch = a * 4 + ain;
        const float volts = raw * kAdcLsbV;
        const float alpha = ch == CH_V2010 ? kFilterAlphaField : kFilterAlpha;
        v[ch] = have[ch] ? v[ch] + alpha * (volts - v[ch]) : volts;
        have[ch] = true;
      }
    }

    portENTER_CRITICAL(&g_mux);
    const bool reset = g_resetMinMax;
    g_resetMinMax = false;
    for (uint8_t ch = 0; ch < kNumAdcCh; ch++) {
      const bool chOk = ok[ch / 4];
      g_snap.v[ch] = chOk ? v[ch] : NAN;
      if (reset || isnan(g_snap.vmin[ch])) {
        g_snap.vmin[ch] = g_snap.v[ch];
        g_snap.vmax[ch] = g_snap.v[ch];
      } else if (chOk) {
        g_snap.vmin[ch] = min(g_snap.vmin[ch], v[ch]);
        g_snap.vmax[ch] = max(g_snap.vmax[ch], v[ch]);
      }
    }
    for (uint8_t a = 0; a < 4; a++) {
      if (!ok[a]) {
        g_snap.errors[a]++;
        for (uint8_t i = 0; i < 4; i++) {
          have[a * 4 + i] = false;  // restart the filter when it comes back
        }
      }
      g_snap.ok[a] = ok[a];
    }
    g_snap.scans++;
    portEXIT_CRITICAL(&g_mux);

    const uint32_t now = millis();
    if (now - lastRateMs >= 2000) {
      g_snap.scanHz = (g_snap.scans - scansAtRate) * 1000.0f / (now - lastRateMs);
      scansAtRate = g_snap.scans;
      lastRateMs = now;
    }
    if (!ok[0] && !ok[1] && !ok[2] && !ok[3]) {
      vTaskDelay(pdMS_TO_TICKS(500));  // no ADCs (bare dev board): don't hammer the bus
    }
  }
}

void adcBegin() {
  for (uint8_t ch = 0; ch < kNumAdcCh; ch++) {
    g_snap.v[ch] = g_snap.vmin[ch] = g_snap.vmax[ch] = NAN;
  }
  Wire.begin(kPinSda, kPinScl, kI2cHz);
  Wire.setTimeOut(20);
  xTaskCreatePinnedToCore(adcTask, "adc", 4096, nullptr, 3, nullptr, 1);
}

void adcGet(AdcSnapshot& out) {
  portENTER_CRITICAL(&g_mux);
  out = g_snap;
  portEXIT_CRITICAL(&g_mux);
}

void adcResetMinMax() {
  g_resetMinMax = true;
}

String adcI2cScan() {
  // Runs alongside the ADC task; Wire serialises bus access.
  String s;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      char buf[8];
      snprintf(buf, sizeof(buf), "0x%02X ", addr);
      s += buf;
    }
  }
  return s.length() ? s : String("none");
}
