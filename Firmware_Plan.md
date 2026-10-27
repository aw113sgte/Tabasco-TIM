# ESP32 TIM Replacement: Firmware Plan (rev 2, 2026-10-27)

Planning only. No code is written until Alex agrees the plan. Recommendations are marked **Rec**.

## 0. Decisions from Alex (2026-10-27)
- **Goal right now: prove every part of the rev A TIM board works.** Everything else comes later.
- **The TIM serves everything.** It hosts the web page for all modules. Later it will also send whatever data the cab display asks for.
- **Cab display (later):** a Waveshare-style ESP32-P4-WIFI6 7/8/10.1" HMI touch board (P4 + C6). It isn't installed yet, so it's out of scope for now.
- **"CAN" data means J1587 data** on the J1708 pair this board already has. No CAN hardware is needed.
- **The MIC is still in the truck, but ignore it for now.** The board **never transmits** on J1708 at this stage. Talking to the MIC needs a scope/log capture first.
- **Networking:** the TIM's own password-protected hotspot, plus shop Wi-Fi when in range (see the channel note in section 2).
- **Units:** psi and °F. No alarms or logging for now.
- **Oil-filter sensors (3246334):** gauge or absolute is unknown. The first engine-off reading will tell (≈0 psi means gauge, ≈14.7 psi means absolute).
- **Software (checked in the BMS project):** BMS v2 is one Arduino IDE sketch (`BMS_v2.ino`) on **Arduino ESP32 core 3.3.x**. It uses built-in `Preferences`, `WiFi` and `esp_now`, and runs ESP-NOW on **fixed channel 1, unencrypted**: JSON status broadcasts every second, plus plain-ASCII commands (`BMS_v2_interface.md`). The TIM firmware follows the same conventions, so the TIM can show BMS data with no change to the BMS.

Hardware this plan targets: rev A board with an ESP32-DevKitC-32E (or -32UE), WROOM-32E, 4 MB flash, 520 KB RAM, dual core, Wi-Fi 2.4 GHz + Bluetooth 4.2/BLE. Pin use and the two firmware must-dos (invert UART2 TX before routing it to IO17; keep the I²C SDA hold time ≥100 ns) are in `Design_Notes.md`.

## 1. What the firmware has to do

1. **Read the board:** 4 × ADS1115 (16 channels) over I²C, plus the C58 switch input. Convert to engineering units (volts, amps, psi, inHg), apply calibration, filter.
2. **Be the TIM on J1708/J1587** (later, ignored for now): answer the MIC's Perform Test #89/#91, Perform Offset and Take Reading. The message framing isn't captured yet, so this starts as **listen-only logging**.
3. **Listen to other J1587 traffic** on the same pair. Engine and transmission ECUs usually broadcast J1587 PIDs on J1708 (RPM, coolant temp, oil pressure, etc.). This is free data, and the first thing to check in the log.
4. **Talk to the other ESP32 modules** wirelessly (ESP-NOW).
5. **User interface:** live values, choose which sensors show, calibrate, change settings.
6. **Wireless updates** from the Galaxy Fold, Linux Mint or Windows 11, without installing anything special.

Rule for the whole design: the wireless side must never stall the sensor reading or the J1708 bus. J1708 and the ADC loop run on core 1 at high priority; Wi-Fi, web and ESP-NOW run on core 0.

## 2. User interface: web server vs Bluetooth

| | Web server over Wi-Fi | Bluetooth (BLE) |
|---|---|---|
| Phone | Any browser, nothing to install | Needs an app (custom, or a generic BLE app with no real UI). Web Bluetooth works in Chrome on Android but not on Firefox/iOS |
| Linux / Windows | Any browser | Chrome Web Bluetooth only; Linux BLE support is patchy |
| Live values | WebSocket push, ~10 updates/s easily | Fine for a few values, slow for a full dashboard |
| Firmware update | Upload the .bin in the browser (~1–2 min) | Possible but slow (several minutes) and needs an app |
| Range | ~30–50 m (better with the -32UE external antenna) | ~10–30 m |
| Memory cost | Moderate | BLE + Wi-Fi together uses ~100 KB more RAM and complicates ESP-NOW timing |

**Rec: web server only.** Skip Bluetooth on this module. One web page works on every device Alex has, it gives the easiest OTA, and it saves the RAM that Bluetooth would cost. Bluetooth can be revisited later if there's a specific need (e.g., a dedicated phone app).

### How the phone or laptop connects (the truck has no Wi-Fi)
- **Option A, TIM makes its own hotspot (SoftAP):** e.g., network `TIM-Tabasco`, password protected, page at `http://192.168.4.1` or `http://tim.local`. Always works in the field. **Rec.**
- **Option B, TIM joins a network (station mode):** home/shop Wi-Fi or the phone's hotspot. Handy in the garage: the laptop stays on its normal network.
- **Rec: hotspot always on, fixed on channel 1; shop Wi-Fi optional and off by default.** ESP-NOW only works on the channel the radio is tuned to, and the BMS is fixed on channel 1. When the TIM joins shop Wi-Fi, its radio moves to the router's channel, and the BMS link drops unless that router is on channel 1. So the firmware gets a setting: "Join shop Wi-Fi" (off by default), with a warning on the page when the router's channel isn't 1. Day to day, the phone or laptop just joins the `TIM-Tabasco` hotspot. A laptop on the hotspot has no internet while it's connected, which is fine for updates.

### Web UI pages
- **Dashboard:** tiles for the sensors Alex picked, in the order picked, with units, min/max since reset, and warning colours.
- **All signals:** every raw and converted value (ADC volts, ratios), for bench work.
- **Sensors:** turn each one on/off for the dashboard, rename, choose units, set alarm limits, reorder.
- **Calibrate:** see section 5.
- **J1708 monitor:** live bus traffic, decoded where known; download a capture log.
- **Modules:** the other ESP-NOW nodes, their status and signal strength.
- **Settings / Update:** Wi-Fi, passwords, firmware upload, backup/restore of settings as a file.

The page is stored compressed in the ESP32's flash (~100–200 KB), so it works with no internet connection.

## 3. Wireless firmware updates (OTA)

Flash layout on the 4 MB part: two app slots of ~1.6 MB each (update into the spare one, then switch), plus storage for settings and the web page.

| Method | From | Notes |
|---|---|---|
| **Browser upload** on the Update page | Fold, Linux, Windows | Pick the `.bin` file, it uploads, checks and reboots. Nothing to install. **Rec: primary** |
| PlatformIO / `espota` push | Linux or Windows dev PC | One click from the editor after a build. Good during development |
| Pull from a URL (e.g., GitHub release) | The ESP32 itself | Needs internet via the shop Wi-Fi. Optional, later |
| Relayed over ESP-NOW | One module to another | For modules that never join Wi-Fi. Slow (minutes) but works. Later phase |

Safety built in:
- **Automatic rollback:** a new firmware that fails its self-test (ADCs not found, crash loop) reverts to the previous one on the next boot. A bad update can't brick it; USB is the last resort.
- **Update blocked while the engine is running**, password required.
- Each module shows its firmware version on the Modules page.

## 4. Module-to-module link: ESP-NOW

ESP-NOW sends short packets (up to 250 bytes; 1470 bytes with the newer v2) directly between ESP32s with no router, latency a few ms, and works alongside Wi-Fi. Classic ESP32, S3, C3, C6 and C5 all speak it, so mixed boards are fine.

Proposed design (matches the BMS v2 conventions):
- **Message format:** JSON under 250 bytes, e.g. `{"type":"tim","v_1437_mv":27410,...}`, broadcast once a second, the same style as the BMS `status`/`board` packets. Plain-ASCII commands (`STATUS`, `GET`, ...) to a node's MAC, with replies back to the sender. The TIM parses the BMS packets, so BMS values show up as "Remote" sensors on the TIM's dashboard.
- **Security:** unencrypted on channel 1, like the BMS, for now. Encryption can be added to every node together later.
- **Discovery:** each node announces itself every few seconds; the Modules page lists them.
- **Channel:** all nodes must be on the same Wi-Fi channel. If the TIM joins the shop Wi-Fi, its channel is the router's. Nodes scan for the TIM's announcement to find it.
- **Hub:** one node collects everyone's data and serves the combined dashboard. The TIM can be the hub, or a display module can be.

### The P4 + C6 boards
The ESP32-P4 has no radio. On P4 + C6 combo boards the C6 is a Wi-Fi/BLE co-processor running Espressif's **ESP-Hosted** firmware, linked to the P4 over SDIO/SPI. Points to check:
- ESP-NOW on those boards depends on the ESP-Hosted version on the C6. Recent ESP-Hosted-MCU releases add ESP-NOW support, but many boards ship with older C6 firmware that only does plain Wi-Fi. I'll confirm the current state for your exact boards before committing to it.
- Updating the C6 is usually done from the P4 (ESP-Hosted has a co-processor OTA path) or through a separate header/USB on the C6. Depends on the board.
- **Fallback if ESP-NOW over ESP-Hosted is a pain:** the P4 talks normal Wi-Fi to the hub (a WebSocket or UDP link over the TIM's hotspot). Same data, slightly more latency, and no C6 firmware work.

## 5. Sensor calibration (engine off)

Engine-off detection, so calibration is only offered when it's valid:
- engine RPM = 0 from J1587 (PID 190) if the engine broadcasts it, **and**
- alternator B+ (pin 13) below ~26 V (not charging) and starter shunt ≈ 0 A.
- The Calibrate button is greyed out with the reason shown if any check fails.

What "zero" means per channel:

| Channel | Engine-off truth | Stored as |
|---|---|---|
| Air-cleaner restriction (C63) | 0 inH₂O | zero offset |
| Oil-filter inlet / outlet pressure (C62 / C61) | 0 psi (if gauge sensors) | zero offset |
| Fuel-inlet sensor (C59), 0–30 psia **absolute** | barometric pressure, ~14.7 psia | **barometric reference**, not a zero. Fuel vacuum = baro − reading. Also checks the assumed sensor curve (expects r ≈ 0.49) |
| Starter shunt current | 0 A | zero offset (INA240 idle, nominally 1.65 V) |
| Vehicle voltages | real voltage, not zero | optional gain trim against a meter: type in the meter reading |
| C58 switch | closed | no calibration; shows state and open-wire fault |

Also:
- "Calibrate all" plus per-sensor buttons; a sanity limit refuses a zero that's far from expected (e.g., a sensor that's unplugged).
- Every calibration stores date/time and the old value, with "undo" and "factory reset".
- Optional two-point calibration (zero and span) for any sensor with a known reference.

## 6. Sensor registry: choosing what's displayed

Every value, wherever it comes from, is one entry in a single list:

```
id, name, source, units, scale, calibration, alarm low/high, shown on dashboard (yes/no), order
```

Sources:
- **Direct:** this board's ADC channels and the C58 switch.
- **J1587:** a PID decoded from J1708 bus traffic (engine/transmission broadcasts).
- **Remote:** a value received over ESP-NOW from another module.
- **Computed:** e.g., oil-filter pressure drop (inlet − outlet), fuel vacuum (baro − C59), alternator charging status.

The Sensors page lists them all with a checkbox and drag-to-reorder. Settings live in flash, survive updates, and can be exported/imported as a file.

J1939/CAN isn't needed: Alex's "CAN" values are the J1587 PIDs on the J1708 pair. The board has no CAN transceiver anyway.

## 7. Software stack

| Option | Pros | Cons |
|---|---|---|
| **PlatformIO + Arduino-ESP32 3.x** (built on ESP-IDF 5) | Fastest to write, lots of libraries (async web server, ADS1115), easy for you to read and change. Works in VS Code on Linux and Windows | A bit less control; ESP-NOW OTA and rollback need some IDF calls (available from Arduino) |
| **ESP-IDF 5.x** (Espressif's native) | Full control, Espressif's esp-now component (with OTA and security), best for P4/ESP-Hosted | More code, steeper learning curve |

**Rec: Arduino ESP32 core 3.3.x, the same as the BMS**, written so it opens and builds in the **Arduino IDE** (board "ESP32 Dev Module", partition scheme "Default 4MB with spiffs", which has two OTA slots). It also builds with `arduino-cli`, so it can be compiled here and checked before you get it. To keep setup simple, it uses only what ships with the core: `WebServer`, `Update`, `Preferences`, `WiFi`, `esp_now`, `Wire`. It needs no extra libraries, and its small ADS1115 driver lives in the sketch. The web page is compiled into the firmware, so there's no separate filesystem upload. Each build also produces the `.bin` you upload over the air.

Code location: `~/Documents/Tabasco/Modules/TIM/Firmware/TIM_ESP32/`. A GitHub repo is optional: I can't create one, but if you make an empty repo I'll use it.

## 8. Build phases

**Phase 1 (now): board verification firmware.** One sketch, flashed once over USB. After that, every update goes over the air.
- Hotspot, web page, browser OTA with rollback, USB serial console (same `HELP`-style commands as the BMS).
- **Self-test page:** each ADS1115 found at 0x48/0x49/0x4A/0x4B (pass/fail), 2.500 V reference, +3.3 V, SENS_5V, +5V_SYS, ESP32 temperature, Wi-Fi RSSI.
- **All 16 channels:** raw ADC volts and converted values side by side (V, A, psi, inHg), C58 open/closed/open-wire, J1708 RX activity.
- **J1587 listen-only:** decode MID/PID frames, check checksums, show the live list of PIDs seen (RPM, coolant °F, oil psi, ...), and save a capture to download. Transmit stays physically disabled: DE is held low because UART TX is never enabled.
- Engine-off calibration and choosing which sensors are shown, in psi and °F.
- Shows the BMS ESP-NOW packets, which proves the radio link with no extra hardware.

**Bench test procedure** (written alongside the code): feed known voltages from a bench supply into the FE-A inputs and compare with a meter; put 0.5/2.5/4.5 V on the FE-B sensor inputs; short and open C58; zero the shunt.

**Later:**
2. Cab display (P4 + C6): the TIM answers its data requests over ESP-NOW or Wi-Fi.
3. MIC / TIM emulation on J1587, after a scope/log capture.
4. Alarms, logging, ESP-NOW encryption.

## 9. Still open
- GitHub repo, yes or no (optional).
- Oil-filter sensor type: answered by the first engine-off reading.
