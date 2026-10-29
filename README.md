# TIM ESP32 firmware

Firmware for the rev A TIM replacement board (design: `Design_Notes.md` in the TIM `PCB` folder) with an **ESP32-DevKitC-32E** or **-32UE** plugged in.
Phase 1 is board verification: it reads everything, calibrates, shows it all in a web page, and listens to J1587. It **never transmits on J1708**: IO17 (transceiver DE) is held low the whole time.

Plan and decisions: `Firmware_Plan.md`. Bench checks: `Bench_Test.md`.

## Folder
| Path | What |
|---|---|
| `TIM_ESP32/` | Arduino sketch. Open `TIM_ESP32.ino` in the Arduino IDE; the other files open as tabs |
| `bin/TIM_ESP32_v<version>.bin` | Compiled app, for the browser Update page (OTA). Use the newest version |
| `bin/TIM_ESP32_v<version>_merged.bin` | Full 4 MB image (bootloader + partitions + app) for a first flash at address 0x0 without the Arduino IDE |

| Sketch file | Does |
|---|---|
| `config.h` | Pins, front-end scaling constants, Wi-Fi defaults |
| `app.cpp` | `setup()`/`loop()`, USB serial console, status LED, OTA rollback confirmation |
| `adc.cpp` | Four ADS1115s (0x48-0x4B), single-shot scan about 50 times a second, filtering, min/max |
| `signals.cpp` | Volts / amps / psi from the ADC channels, engine state (J1587 engine speed only), zero calibration, dashboard min/max |
| `j1587.cpp` | UART2 receive at 9600, J1708 framing and checksum, J1587 PID decoding, capture buffer |
| `web.cpp`, `webpage.h` | Hotspot, web UI, JSON API, firmware upload |
| `espnow_link.cpp` | ESP-NOW on channel 1 (same conventions as the BMS v2) |
| `health.cpp` | Last restart reason; restarts when free memory runs low and nobody is on the hotspot |
| `settings.cpp` | Calibration, Wi-Fi and dashboard settings in flash (they survive updates) |

## Build and first flash (Arduino IDE, once, over USB)
1. Arduino IDE with **esp32 by Espressif Systems 3.3.x** (the same core as the BMS; it was built and checked here with 3.3.12). No extra libraries are needed.
2. Tools: Board **ESP32 Dev Module**, Partition Scheme **Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)**. Leave the rest at the defaults.
3. Plug the DevKitC into the PC by USB (on the board or on its own) and press Upload.
4. Open the Serial Monitor at 115200 and type `HELP`.

A DevKitC on its own (not plugged into the TIM board) runs fine: it reports the ADCs as missing and blinks the status LED fast.

## Using it
- Join the Wi-Fi network **TIM-Tabasco** (password **tabasco-tim**). The phone or laptop pops up the TIM page on its own, the same way public Wi-Fi shows a sign-in page. You can also open **http://192.168.4.1** (or http://tim.local on a laptop) at any time. For firmware uploads, use Chrome at http://192.168.4.1 if the pop-up window's file picker doesn't work.
- Change the hotspot password on the Settings page. Settings, Calibrate and Update ask for a login: user `admin`, password = the hotspot password.
- Dashboard tiles show min and max since restart, each with the uptime second it happened. **Clear min/max** resets all of them (also the ADC min/max). Nothing is saved.
- Engine state comes only from engine speed on J1587: over 300 rpm = running, below = off, nothing heard = unknown. Updates and zeroing work at any time; zero with the engine off for a true zero.
- Tabs: Dashboard (sensors you pick), All signals (every ADC channel raw and converted, min/max), J1587 (trouble codes the modules broadcast, every parameter heard on the bus, download a capture), Calibrate, Modules (ESP-NOW nodes heard), Settings (dashboard choice, firmware update, Wi-Fi, restart, factory reset).
- USB serial commands: `STATUS`, `RAW`, `I2C`, `J1587 ON|OFF` (print every frame), `ZERO all|shunt|c59|oil_in|oil_out|air`, `WIFI` (status), `WIFI STA OFF` / `WIFI STA ON` / `WIFI RETRY` (shop Wi-Fi), `SCAN` (networks the TIM can hear), `NOW`, `REBOOT`.
- Shop Wi-Fi: 2.4 GHz only. If the TIM can't join within 20 s, it gives up, puts the hotspot back on channel 1, shows the reason in Settings (network not found, password rejected, ...) and retries after 30 s, doubling up to 10 min while the network stays away (at least 5 min while someone is on the hotspot, because a try can move the hotspot off channel 1 briefly). The hotspot itself is never restarted. Settings shows how often shop Wi-Fi was lost since restart and why. "Channel 1 only" (on by default) joins only an access point on channel 1, so the hotspot and the BMS link stay on channel 1. Settings has a network scan and a "Try shop Wi-Fi now" button.
- LEDs: D5 status blinks once a second with all four ADCs answering, fast when one is missing. D6 lights while J1587 frames are arriving.

## Updating over Wi-Fi
From the phone: ask Claude for the change. Claude pushes the new `.bin` to `bin/` on GitHub. Download it in the phone's browser **before** joining TIM-Tabasco, because the hotspot has no internet, then upload it on the Settings tab.

From a PC with the Arduino IDE:
1. In the Arduino IDE: Sketch > **Export Compiled Binary**. The `.bin` lands in the sketch's `build/` folder (use the plain `TIM_ESP32.ino.bin`, not `merged` or `bootloader`).
2. On the phone or PC, open the Settings tab, choose the file and press **Upload**. The board restarts in about 20 s.
3. A new firmware has to run 30 s before it's kept. If it crashes or resets sooner, the board goes back to the previous firmware by itself. USB flashing always works as a last resort.

## ESP-NOW packets (channel 1, unencrypted, once a second, broadcast)
```json
{"type":"tim_v","v1437":27.41,"v1139":27.38,"v1138":0.01,"v1431":27.40,"v1274":27.39,"v2010":0.52,"v1435a":0.00,"v1435b":0.00,"v2085":0.40,"shunt":0,"sens5v":5.01}
{"type":"tim_p","fw":"0.1.0","c59_psia":14.62,"fuel_vac":0.08,"oil_in":0.00,"oil_out":0.00,"oil_dp":0.00,"air":0.0,"air_psia":13.60,"c58":0,"engine":"off","rpm":null,"j1587_frames":1234}
```
Volts in V, current in A, pressures in psi (C59 in psia). `null` = no reading. `c58`: 0 = closed (normal), 1 = open (restricted or broken wire).
Plain-text commands sent to the TIM's MAC: `STATUS` / `GET` (both packets back to the sender), `PING`, `HELP`.

## Known limits (phase 1)
- C63 air cleaner is a Sensata 67CP022-0015AFNA2 (from the label): 0-15 psia absolute, 0.5-4.5 V. Restriction = engine-off reference minus the reading, in inH2O ("air"; ESP-NOW too); "air_psia" is the raw pressure. Zero it with the engine off to store the reference; until then C59's barometric value is used. Idle restriction is a fraction of an inH2O, so it only moves under load.
- The oil-filter (C61/C62) sensor range is unknown. Its curve is a placeholder (0-100 psi over the usual 0.5-4.5 V). Set the real curve on the Calibrate page once it's known. C59 uses the catalog 0-30 psia with the usual 0.5-4.5 V output, which isn't confirmed yet. The engine-off reading checks it: it should be about 14.7 psia, ratio about 0.49.
- J1587 scalings come from the SAE J1587 table. Check them against a known gauge. Parameters without a known scaling can still go on the dashboard as a raw number.
- Trouble codes: only the ones the modules broadcast (PID 194). Reading stored codes and clearing codes both need transmitting, which is a later step.
- No MIC / TIM emulation yet: that needs a bus capture first. The J1587 tab's download gives exactly that capture.
