// TIM ESP32 replacement firmware, phase 1: board verification.
// Board: rev A TIM replacement PCB with an ESP32-DevKitC-32E (or -32UE). Arduino ESP32 core 3.3.x,
// board "ESP32 Dev Module", partition scheme "Default 4MB with spiffs" (two OTA slots). No extra libraries.
// Design: Design_Notes.md (pin map, front-end scaling) and Firmware_Plan.md.
//
// - Reads the four ADS1115s and the C58 switch, converts to V / A / psi, engine-off calibration.
// - Listens to J1708/J1587 (never transmits: IO17 / DE is held low).
// - Wi-Fi hotspot "TIM-Tabasco" with the web UI at http://192.168.4.1, browser firmware update with rollback.
// - ESP-NOW broadcasts on channel 1, the same conventions as the BMS v2.
// - USB serial console at 115200 (type HELP).

// setup() and loop() are in app.cpp; the other .cpp files hold each part of the firmware.
