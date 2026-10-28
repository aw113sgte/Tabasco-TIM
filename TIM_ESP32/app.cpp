// Start-up, main loop, serial console, status LED and OTA rollback confirmation.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_ota_ops.h>

#include "adc.h"
#include "config.h"
#include "espnow_link.h"
#include "j1587.h"
#include "settings.h"
#include "signals.h"
#include "uptime.h"
#include "web.h"

static bool g_appConfirmed = false;

// Keep a new OTA image "pending" until it has run for kRollbackConfirmMs (see confirmFirmware()).
// If it crashes or resets before then, the bootloader goes back to the previous firmware.
bool verifyRollbackLater() {
  return true;
}

static void confirmFirmware() {
  if (g_appConfirmed || millis() < kRollbackConfirmMs) {
    return;
  }
  g_appConfirmed = true;
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("[TIM] OTA: new firmware confirmed");
  }
}

// ------------------------------------------------------------------ serial console

static void printStatus() {
  AdcSnapshot a;
  SigValue s[kNumSig];
  adcGet(a);
  signalsCompute(a, s);
  const EngineInfo e = engineInfo(s);
  Serial.printf("TIM firmware %s, up %lu s, free heap %u\n", TIM_FW_VERSION, (unsigned long)uptimeSeconds(),
                (unsigned)ESP.getFreeHeap());
  Serial.printf("ADCs: U7 %s, U8 %s, U9 %s, U10 %s, %.1f scans/s\n", a.ok[0] ? "ok" : "MISSING",
                a.ok[1] ? "ok" : "MISSING", a.ok[2] ? "ok" : "MISSING", a.ok[3] ? "ok" : "MISSING", a.scanHz);
  Serial.printf("Engine: %s (%s)\n", e.state == ENG_OFF ? "off" : e.state == ENG_RUNNING ? "running"
                                     : e.state == ENG_CRANKING ? "cranking" : "unknown", e.reason);
  for (uint8_t i = 0; i < kNumSig; i++) {
    Serial.printf("  %-40s %10.*f %-5s %s\n", kSig[i].name, kSig[i].decimals + 1, s[i].v, kSig[i].unit,
                  kSigStatusText[s[i].st]);
  }
  J1587Stats js;
  j1587GetStats(js);
  Serial.printf("J1587: %lu frames, %lu bad checksum, %lu malformed, %lu UART errors\n", (unsigned long)js.frames,
                (unsigned long)js.badChecksum, (unsigned long)js.malformed, (unsigned long)js.uartErrors);
  Serial.printf("WiFi: %s\n", wifiStatusText().c_str());
}

static void printRaw() {
  AdcSnapshot a;
  adcGet(a);
  for (uint8_t ch = 0; ch < kNumAdcCh; ch++) {
    Serial.printf("  %-11s pin %-16s %.4f V (min %.4f max %.4f)\n", kAdcChName[ch], kAdcChPin[ch], a.v[ch],
                  a.vmin[ch], a.vmax[ch]);
  }
}

static void handleSerial() {
  static char line[64];
  static uint8_t len = 0;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (len < sizeof(line) - 1) line[len++] = c;
      continue;
    }
    if (!len) continue;
    line[len] = '\0';
    len = 0;
    char* arg = strchr(line, ' ');
    if (arg) *arg++ = '\0';
    if (!strcasecmp(line, "HELP")) {
      Serial.println("Commands: STATUS, RAW, I2C, J1587 [ON|OFF], ZERO <all|shunt|c59|oil_in|oil_out|air>, "
                     "WIFI [STA ON|STA OFF|RETRY], SCAN, NOW, REBOOT");
    } else if (!strcasecmp(line, "STATUS") || !strcasecmp(line, "GET")) {
      printStatus();
    } else if (!strcasecmp(line, "RAW")) {
      printRaw();
    } else if (!strcasecmp(line, "I2C")) {
      Serial.printf("I2C devices: %s (expect 0x48 0x49 0x4A 0x4B)\n", adcI2cScan().c_str());
    } else if (!strcasecmp(line, "J1587")) {
      if (arg) j1587SetEcho(!strcasecmp(arg, "ON"));
      Serial.printf("J1587 frame echo %s\n", j1587Echo() ? "on" : "off");
    } else if (!strcasecmp(line, "ZERO")) {
      const String err = calZero(arg ? arg : "all", 0);
      Serial.println(err.length() ? "Refused: " + err : String("Zeroed"));
    } else if (!strcasecmp(line, "WIFI")) {
      if (arg && !strcasecmp(arg, "STA OFF")) {
        wifiSetSta(false);
        Serial.println("Shop Wi-Fi off (saved); hotspot only, channel 1");
      } else if (arg && !strcasecmp(arg, "STA ON")) {
        wifiSetSta(true);
      } else if (arg && !strcasecmp(arg, "RETRY")) {
        wifiRetryNow();
      }
      Serial.printf("%s\n", wifiStatusText().c_str());
    } else if (!strcasecmp(line, "SCAN")) {
      static String names[20];
      static int8_t rssi[20];
      static uint8_t chan[20];
      static bool secure[20];
      const int n = wifiScan(names, rssi, chan, secure, 20);
      Serial.printf("%d networks (2.4 GHz only):\n", n);
      for (int i = 0; i < n; i++) {
        Serial.printf("  %-32s ch %2u  %4d dBm  %s\n", names[i].c_str(), chan[i], rssi[i], secure[i] ? "secured" : "open");
      }
    } else if (!strcasecmp(line, "NOW")) {
      const NowStats st = espnowStats();
      Serial.printf("ESP-NOW %s: tx %lu, fail %lu, rx %lu, commands %lu\n", st.ready ? "ready" : "NOT ready",
                    (unsigned long)st.tx, (unsigned long)st.txFail, (unsigned long)st.rx, (unsigned long)st.commands);
    } else if (!strcasecmp(line, "REBOOT")) {
      ESP.restart();
    } else {
      Serial.println("Unknown command, type HELP");
    }
  }
}

// ------------------------------------------------------------------ status LED
// Slow blink = all four ADCs answering; fast blink = at least one missing.

static void serviceLed(const AdcSnapshot& a) {
  const bool allOk = a.ok[0] && a.ok[1] && a.ok[2] && a.ok[3];
  const uint32_t period = allOk ? 1000 : 200;
  digitalWrite(kPinLedStatus, (millis() % period) < period / 2 ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200);
  pinMode(kPinLedStatus, OUTPUT);
  j1587Begin();  // first, so DE is driven low straight away
  Serial.printf("\n[TIM] firmware %s starting\n", TIM_FW_VERSION);
  settingsBegin();
  adcBegin();
  sigMinMaxBegin();
  wifiBegin();
  espnowBegin();
  webBegin();
  Serial.println("[TIM] ready: join the hotspot and open http://192.168.4.1 (type HELP here for commands)");
}

void loop() {
  webLoop();
  handleSerial();

  static uint32_t lastSig = 0;
  static AdcSnapshot a;
  static SigValue s[kNumSig];
  static EngineInfo e;
  if (millis() - lastSig >= 100) {
    lastSig = millis();
    adcGet(a);
    signalsCompute(a, s);
    e = engineInfo(s);
  }
  espnowLoop(s, e);
  serviceLed(a);
  confirmFirmware();
  delay(2);
}
