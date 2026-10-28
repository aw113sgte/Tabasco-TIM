// Seconds since boot from the 64-bit esp_timer, so it doesn't wrap at 49.7 days like millis().
#pragma once

#include <esp_timer.h>

inline uint32_t uptimeSeconds() {
  return (uint32_t)(esp_timer_get_time() / 1000000LL);
}
