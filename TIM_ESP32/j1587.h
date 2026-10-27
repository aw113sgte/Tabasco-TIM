// SAE J1708 receive (UART2) and J1587 decoding. Listen-only: the transmitter is held off.
#pragma once

#include <Arduino.h>

struct J1587Pid {
  uint8_t mid;
  uint16_t pid;      // 256+ = page 2 (after PID 255)
  uint8_t len;       // data bytes stored (variable-length PIDs: without the count byte)
  uint8_t data[16];
  uint32_t lastMs;
  uint32_t count;
};

struct J1587Stats {
  uint32_t bytes;
  uint32_t frames;       // good checksum
  uint32_t badChecksum;
  uint32_t malformed;    // good checksum but the PID lengths don't add up
  uint32_t tooLong;      // longer than the J1708 21-byte limit (still decoded)
  uint32_t uartErrors;   // framing / overflow / break
  uint32_t lastFrameMs;
};

void j1587Begin();
void j1587GetStats(J1587Stats& out);
int j1587CopyPids(J1587Pid* out, int max);
bool j1587Value(uint8_t mid, uint16_t pid, float& value, uint32_t maxAgeMs);
bool j1587Decode(const J1587Pid& p, float& value);  // false = no known scaling
const char* j1587PidName(uint16_t pid);             // nullptr = unknown
const char* j1587PidUnit(uint16_t pid);
uint8_t j1587PidDecimals(uint16_t pid);
const char* j1587MidName(uint8_t mid);
void j1587CaptureText(String& out);                 // last frames as text, oldest first
void j1587Clear();
void j1587SetEcho(bool on);                         // print every frame on USB serial
bool j1587Echo();
