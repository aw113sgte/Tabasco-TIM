// TIM ESP32 replacement firmware: pins and fixed constants.
// Board: rev A TIM replacement PCB with an ESP32-DevKitC-32E (or -32UE). Pin map: Design_Notes.md, "ESP32 GPIO".
#pragma once

#include <Arduino.h>

#define TIM_FW_VERSION "0.1.3"

// ------------------------------------------------------------------ pins
static const int8_t kPinJ1708Rx = 16;   // UART2 RX <- SN65HVD1781 RO
static const int8_t kPinJ1708De = 17;   // transceiver DE. Phase 1 holds it LOW (bus released, never transmits)
static const int8_t kPinSda = 21;
static const int8_t kPinScl = 22;
static const int8_t kPinAdcAlert = 34;  // ADS1115 ALRT, wired-OR, 10k pull-up (not used: the scan polls on a timer)
static const int8_t kPinLedStatus = 4;  // D5, active high
static const int8_t kPinLedBus = 13;    // D6 J1587 activity, active high

// ------------------------------------------------------------------ ADC
static const uint32_t kI2cHz = 400000;  // default ESP32 SDA hold time is fine; U9 decodes its address from SDA
static const uint8_t kAdcAddr[4] = {0x48, 0x49, 0x4A, 0x4B};  // U7, U8, U9, U10
static const float kAdcLsbV = 4.096f / 32768.0f;             // PGA +/-4.096 V

// FE-A (vehicle voltages): 100k in, 60.4k to the 2.500 V reference, 11.5k to ground.
// V_adc = kFeAGain * V_in + kFeAOffset (an open input reads about +0.4 V).
static const float kFeAGain = 0.088096f;
static const float kFeAOffset = 0.364640f;
// FE-B (sensor inputs and the SENS_5V monitor): 22k / 33k.
static const float kFeBGain = 0.6f;
// FE-C (shunt): INA240A1 gain 20 on a 50 uOhm shunt = 1 mV per amp, idling at 1.65 V.
static const float kShuntVPerA = 0.001f;
static const float kShuntIdleV = 1.65f;
// C58 switch (4.7k pull-up, 2.0k series, FE-B divider): closed ~0 V, open ~2.7 V.
static const float kC58OpenV = 1.35f;

// ------------------------------------------------------------------ engine state
static const float kAltChargingV = 26.0f;  // alternator B+ above this = charging = engine running
static const float kCrankingA = 50.0f;     // |starter current| above this = cranking
static const uint16_t kRunningRpm = 300;

// ------------------------------------------------------------------ J1708 / J1587
static const uint32_t kJ1708Baud = 9600;
static const uint8_t kJ1708MaxFrame = 64;  // J1708 allows 21; longer frames are kept and counted

// ------------------------------------------------------------------ wireless
static const uint8_t kWifiChannel = 1;  // fixed: the BMS ESP-NOW link runs on channel 1
static const char* const kDefaultApSsid = "TIM-Tabasco";
static const char* const kDefaultApPass = "tabasco-tim";  // change on the Settings page (8+ characters)
static const char* const kHostname = "tim";               // http://tim.local when the device supports mDNS
static const uint32_t kEspNowPeriodMs = 1000;
static const uint32_t kRollbackConfirmMs = 30000;  // new firmware must run this long before it is kept
