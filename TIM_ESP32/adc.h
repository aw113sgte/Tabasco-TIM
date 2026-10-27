// Four ADS1115 ADCs (U7-U10) on I2C, scanned by a background task.
#pragma once

#include <Arduino.h>

// ADC channel = 4 * ADC index + AIN number (Design_Notes.md, C55 pin map).
enum AdcCh : uint8_t {
  CH_SHUNT_I, CH_V1139, CH_V1138, CH_V1431,    // U7 0x48
  CH_V1274, CH_V2010, CH_V1435A, CH_V1437,     // U8 0x49
  CH_S_C59, CH_S_C61, CH_S_C62, CH_S_C63,      // U9 0x4A
  CH_SENS5V, CH_C58, CH_V2085, CH_V1435B,      // U10 0x4B
  kNumAdcCh
};

struct AdcSnapshot {
  float v[kNumAdcCh];     // filtered volts at the ADC pin
  float vmin[kNumAdcCh];
  float vmax[kNumAdcCh];
  bool ok[4];             // ADC answered on the last scan
  uint32_t errors[4];     // I2C failures since boot
  uint32_t scans;
  float scanHz;
};

extern const char* const kAdcChName[kNumAdcCh];
extern const char* const kAdcChPin[kNumAdcCh];  // C55 pin and wire, e.g. "1 / 1437"

void adcBegin();                 // Wire + task start
void adcGet(AdcSnapshot& out);   // thread-safe copy
void adcResetMinMax();
String adcI2cScan();             // list of addresses that answer, for the serial console
