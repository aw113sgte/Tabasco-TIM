// SAE J1708 receive (UART2) and J1587 decoding. Listen-only: the transmitter is held off.
#pragma once

#include <Arduino.h>

static const int kJ1587PidSlots = 128;
static const int kJ1587DtcSlots = 32;
static const uint32_t kJ1587StaleMs = 60000;  // a full table reuses slots not heard for this long

struct J1587Pid {
  uint8_t mid;
  uint16_t pid;      // 256+ = page 2 (after PID 255)
  uint8_t len;       // data bytes stored (variable-length PIDs: without the count byte)
  uint8_t data[16];
  uint32_t lastMs;
  uint32_t count;
  float vmin, vmax;  // dashboard min/max since boot or the last clear (NAN = none yet)
  uint32_t tmin, tmax;  // uptime second each was reached
};

struct J1587Stats {
  uint32_t bytes;
  uint32_t frames;       // good checksum
  uint32_t badChecksum;
  uint32_t malformed;    // good checksum but the PID lengths don't add up
  uint32_t tooLong;      // longer than the J1708 21-byte limit (still decoded)
  uint32_t uartErrors;   // framing / overflow / break
  uint32_t ignoredMid;   // good checksum but a MID outside 128-254 (corrupted frame or not J1587)
  uint32_t pidDropped;   // parameter table full of live entries, new parameter not stored
  uint32_t dtcDropped;   // same for the trouble-code table
  uint32_t lastFrameMs;
};

// A trouble code from PID 194 (diagnostic code transmission), which each module broadcasts on its own.
struct J1587Dtc {
  uint8_t mid;
  uint16_t num;      // PID or SID number (256+ = page 2 / expansion code)
  bool isSid;        // true = SID (module-specific component), false = PID (a parameter)
  uint8_t fmi;       // failure mode identifier 0-15
  bool active;
  uint8_t count;     // occurrence count, 0 if the module didn't send one
  uint32_t firstMs;
  uint32_t lastMs;
};

void j1587Begin();
void j1587GetStats(J1587Stats& out);
int j1587CopyPids(J1587Pid* out, int max);
bool j1587Value(uint8_t mid, uint16_t pid, float& value, uint32_t maxAgeMs);
bool j1587Decode(const J1587Pid& p, float& value);  // false = no known scaling
bool j1587Number(const J1587Pid& p, float& value, bool& known);  // scaled value, or the raw number of an unknown 1-4 byte PID
void j1587ResetMinMax();
const char* j1587PidName(uint16_t pid);             // nullptr = unknown
const char* j1587PidUnit(uint16_t pid);
uint8_t j1587PidDecimals(uint16_t pid);
const char* j1587MidName(uint8_t mid);
int j1587CopyDtcs(J1587Dtc* out, int max);
const char* j1587FmiText(uint8_t fmi);
void j1587CaptureText(String& out);                 // last frames as text, oldest first
void j1587Clear();
void j1587SetEcho(bool on);                         // print every frame on USB serial
bool j1587Echo();
