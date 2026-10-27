// Small helpers for building JSON text without an extra library.
#pragma once

#include <Arduino.h>

inline void jNum(String& o, float v, uint8_t dec) {
  if (isnan(v) || isinf(v)) {
    o += "null";
  } else {
    o += String(v, (unsigned int)dec);
  }
}

inline void jStr(String& o, const char* s) {
  o += '"';
  for (const char* p = s; p && *p; p++) {
    const char c = *p;
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if ((uint8_t)c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      o += buf;
    } else {
      o += c;
    }
  }
  o += '"';
}

inline void jKey(String& o, const char* k) {
  jStr(o, k);
  o += ':';
}

inline void jMac(String& o, const uint8_t* m) {
  char buf[20];
  snprintf(buf, sizeof(buf), "\"%02X:%02X:%02X:%02X:%02X:%02X\"", m[0], m[1], m[2], m[3], m[4], m[5]);
  o += buf;
}
