// Wi-Fi hotspot (plus optional shop Wi-Fi), the web UI and browser firmware update.
#pragma once

#include <Arduino.h>

void wifiBegin();
void webBegin();
void webLoop();
String wifiStatusText();
