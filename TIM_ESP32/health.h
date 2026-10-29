// Why the board last restarted, and a restart when the heap runs low (rather than the Wi-Fi quietly failing).
#pragma once

void healthLoop();               // call from loop()
const char* resetReasonText();   // reason for the last restart, for the web page and serial STATUS
