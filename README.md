# TIM ESP32 firmware

Firmware for the rev A TIM replacement board (design: `Design_Notes.md` in the TIM `PCB` folder) with an **ESP32-DevKitC-32E** or **-32UE** plugged in.
Phase 1 is board verification: it reads everything, calibrates, shows it all in a web page, and listens to J1587. It **never transmits on J1708**: IO17 (transceiver DE) is held low the whole time.

Plan and decisions: `Firmware_Plan.md`. Bench checks: `Bench_Test.md`.

## Folder
| Path | What |
|---|---|
| `TIM_ESP32/` | Arduino sketch. Open `TIM_ESP32.ino` in the Arduino IDE; the other files open as tabs |
| `bin/TIM_ESP32_v0.1.0.bin` | Compiled app, for the browser Update page (OTA) |
| `bin/TIM_ESP32_v0.1.0_merged.bin` | Full 4 MB image (bootloader + partitions + app) for a first flash at address 0x0 without the Arduino IDE |

| Sketch file | Does |
|---|---|
| `config.h` | Pins, front-end scaling constants, Wi-Fi defaults |
| `app.cpp` | `setup()`/`loop()`, USB serial console, status LED, OTA rollback confirmation |
| `adc.cpp` | Four ADS1115s (0x48-0x4B), single-shot scan about 50 times a second, filtering, min/max |
| `signals.cpp` | Volts / amps / psi from the ADC channels, engine-running check, engine-off calibration |
| `j1587.cpp` | UART2 receive at 9600, J1708 framing and checksum, J1587 PID decoding, capture buffer |
| `web.cpp`, `webpage.h` | Hotspot, web UI, JSON API, firmware upload |
| `espnow_link.cpp` | ESP-NOW on channel 1 (same conventions as the BMS v2) |
| `settings.cpp` | Calibration, Wi-Fi and dashboard settings in flash (they survive updates) |

## Build and first flash (Arduino IDE, once, over USB)
1. Arduino IDE with **esp32 by Espressif Systems 3.3.x** (the same core as the BMS; it was built and checked here with 3.3.12). No extra libraries are needed.
2. Tools: Board **ESP32 Dev Module**, Partition Scheme **Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)**. Leave the rest at the defaults.
3. Plug the DevKitC into the PC by USB (on the board or on its own) and press Upload.
4. Open the Serial Monitor at 115200 and type `HELP`.

A DevKitC on its own (not plugged into the TIM board) runs fine: it reports the ADCs as missing and blinks the status LED fast.

## Using it
- Join the Wi-Fi network **TIM-Tabasco**, password **tabasco-tim**, then open **http://192.168.4.1** (or http://tim.local on a laptop).
- Change the hotspot password on the Settings page. Settings, Calibrate and Update ask for a login: user `admin`, password = the hotspot password.
- Tabs: Dashboard (sensors you pick), All signals (every ADC channel raw and converted, min/max), J1587 (everything heard on the bus, download a capture), Calibrate, Modules (ESP-NOW nodes heard), Settings (dashboard choice, firmware update, Wi-Fi, restart, factory reset).
- USB serial commands: `STATUS`, `RAW`, `I2C`, `J1587 ON|OFF` (print every frame), `ZERO all|shunt|c59|oil_in|oil_out|air`, `WIFI`, `NOW`, `REBOOT`.
- LEDs: D5 status blinks once a second with all four ADCs answering, fast when one is missing. D6 lights while J1587 frames are arriving.

## Updating over Wi-Fi
1. In the Arduino IDE: Sketch > **Export Compiled Binary**. The `.bin` lands in the sketch's `build/` folder (use the plain `TIM_ESP32.ino.bin`, not `merged` or `bootloader`).
2. On the phone or PC, open the Settings tab, choose the file and press **Upload**. The board restarts in about 20 s.
3. A new firmware has to run 30 s before it's kept. If it crashes or resets sooner, the board goes back to the previous firmware by itself. USB flashing always works as a last resort.

## ESP-NOW packets (channel 1, unencrypted, once a second, broadcast)
```json
{"type":"tim_v","v1437":27.41,"v1139":27.38,"v1138":0.01,"v1431":27.40,"v1274":27.39,"v2010":0.52,"v1435a":0.00,"v1435b":0.00,"v2085":0.40,"shunt":0,"sens5v":5.01}
{"type":"tim_p","fw":"0.1.0","c59_psia":14.62,"fuel_vac":0.08,"oil_in":0.00,"oil_out":0.00,"oil_dp":0.00,"air":0.00,"c58":0,"engine":"off","rpm":null,"j1587_frames":1234}
```
Volts in V, current in A, pressures in psi (C59 in psia). `null` = no reading. `c58`: 0 = closed (normal), 1 = open (restricted or broken wire).
Plain-text commands sent to the TIM's MAC: `STATUS` / `GET` (both packets back to the sender), `PING`, `HELP`.

## Known limits (phase 1)
- The oil-filter (C61/C62) and air-cleaner (C63) sensor ranges are unknown. Their curves are placeholders (0-100 psi and 0-1 psi over the usual 0.5-4.5 V). Set the real curve on the Calibrate page once it's known. C59 uses the catalog 0-30 psia with the usual 0.5-4.5 V output, which isn't confirmed yet. The engine-off reading checks it: it should be about 14.7 psia, ratio about 0.49.
- J1587 scalings come from the SAE J1587 table. Check them against a known gauge.
- No MIC / TIM emulation yet: that needs a bus capture first. The J1587 tab's download gives exactly that capture.
