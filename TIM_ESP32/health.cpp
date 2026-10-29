#include "health.h"

#include <Arduino.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_system.h>

// The Wi-Fi driver needs free heap to accept a phone on the hotspot. If something leaks, restart while
// nobody is connected instead of leaving a hotspot that drops every join.
static const uint32_t kLowHeapBytes = 24000;
static const uint32_t kLowHeapCheckMs = 10000;
static const uint32_t kLowHeapMagic = 0x4C4F5748;  // "LOWH"

RTC_NOINIT_ATTR static uint32_t g_lowHeapRestart;  // survives a software restart, not a power cycle
static bool g_wasLowHeap = false;
static bool g_checked = false;

static void checkBoot() {
  if (g_checked) return;
  g_checked = true;
  g_wasLowHeap = esp_reset_reason() == ESP_RST_SW && g_lowHeapRestart == kLowHeapMagic;
  g_lowHeapRestart = 0;
}

const char* resetReasonText() {
  checkBoot();
  if (g_wasLowHeap) return "restarted itself: free memory ran low";
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "reset pin (EN)";
    case ESP_RST_SW: return "software restart (update, settings or Restart button)";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog (firmware hung)";
    case ESP_RST_BROWNOUT: return "brownout (supply dipped)";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default: return "unknown";
  }
}

void healthLoop() {
  checkBoot();
  static uint32_t last = 0;
  if (millis() - last < kLowHeapCheckMs) return;
  last = millis();
  if (ESP.getFreeHeap() < kLowHeapBytes && WiFi.softAPgetStationNum() == 0 && !Update.isRunning()) {
    Serial.printf("[TIM] free heap %u bytes, restarting\n", (unsigned)ESP.getFreeHeap());
    g_lowHeapRestart = kLowHeapMagic;
    delay(100);
    ESP.restart();
  }
}
