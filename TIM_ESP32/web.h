// Wi-Fi hotspot (plus optional shop Wi-Fi), the web UI and browser firmware update.
#pragma once

#include <Arduino.h>

void wifiBegin();
void wifiLoop();                 // shop Wi-Fi join / give-up / retry (called from webLoop)
void wifiRetryNow();
void wifiSetSta(bool enable);    // saves the setting
int wifiScan(String names[], int8_t rssi[], uint8_t chan[], bool secure[], int max);
void webBegin();
void webLoop();
String wifiStatusText();
