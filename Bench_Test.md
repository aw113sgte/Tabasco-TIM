# TIM board bench test (firmware 0.1.0)

Goal: prove every circuit on the rev A board with the phase 1 firmware before it goes on the truck. Work through it in order and write the readings down. The **All signals** tab shows each ADC channel's volts at the ADC pin, its "Input" value (the C55 pin side) and min/max; the serial `RAW` command shows the same.

Equipment: 0-30 V bench supply with current limit, a multimeter, a 1-5 V source for the sensor inputs (a second supply, or a 10k pot across SENS_5V), jumper leads to the C55 header (J1).

Expected values come from the front-end formulas in `Design_Notes.md`:
- FE-A ADC V = 0.0881 × V_in + 0.365, so 0 V in reads 0.365 V and an open input reads about +0.40 V.
- FE-B ADC V = 0.6 × V_in.
- SHUNT_I = 1.65 V + 1 mV per amp.

## 1. Dev board alone
1. Flash over USB (README). Serial shows `[TIM] firmware 0.1.0 starting` and the hotspot line.
2. Join TIM-Tabasco on the phone and open http://192.168.4.1. All tabs load.
3. All signals shows all four ADCs MISSING. That's expected with no TIM board: this step only checks Wi-Fi and the page.
4. **OTA check:** Arduino > Export Compiled Binary, then upload the `.bin` on the Settings tab. The page says Done, the board restarts, and after 30 s the serial shows `OTA: new firmware confirmed`.

## 2. Power-up on the TIM board
Plug the DevKitC in (module side up, USB toward J1, per the board notes). Bench supply at 24 V, **current limit 150 mA**, into C55 pin 1 (+) and pin 3 (−). USB unplugged.

| Check | Expect |
|---|---|
| Supply current | roughly 30-80 mA with Wi-Fi on (record it) |
| +5V_SYS (meter) | 5.0-5.1 V |
| +3V3 board rail (meter) | 3.3 V |
| 2.500 V reference U3 (meter) | 2.500 V ±0.2% |
| C55 pin 10 SENS_5V (meter and `sens5v`) | 5.0 V ±1%; the two agree within 0.02 V |
| D5 status LED | slow blink (all four ADCs answer) |
| Serial `I2C` | `0x48 0x49 0x4A 0x4B` |
| All signals summary | all four ADCs ok, 0 errors, about 40-60 scans/s |

Repeat from pin 2 instead of pin 1 (D2 path). Then reverse the supply polarity briefly: no current should flow (D1/D2).

Undervoltage: lower the supply slowly. The buck should start at about 6.6 V (EN divider). Record where it drops out.

## 3. FE-A voltage inputs (pins 1, 4, 7, 8, 11, 12, 13, 14, 16)
Keep the board powered from pin 1. For each input, put the bench supply's second output (or a divider off the main supply) on the pin, referenced to pin 3:

| Applied | Expected ADC V | Expected "Input" |
|---|---|---|
| open | about 0.40 | about 0.4 V |
| 0 V (to pin 3) | 0.365 | 0.00 V |
| 12.00 V | 1.422 | 12.00 V |
| 24.00 V | 2.479 | 24.00 V |
| 30.00 V | 3.008 | 30.00 V |

Pass: within ±0.15 V before trim. Then do **Calibrate > Voltage trim** at 24 V with the meter reading for each input. The trim should land between 0.98 and 1.02. Pin 1 measures its own supply, so for V_1437 just compare it with the meter on pin 1.

## 4. FE-B sensor inputs (pins 19, 23, 27, 29) and ratio
With SENS_5V on pin 10 at about 5 V, put a variable 0-5 V on each signal pin (pin A of each sensor is ground, pins 20/24/28/30):

| Applied | ADC V | Ratio |
|---|---|---|
| open | 0.000 (status "open / unplugged") | 0 |
| 0.50 V | 0.300 | 0.100 |
| 2.50 V | 1.500 | 0.500 |
| 4.50 V | 2.700 | 0.900 |

The C59 row should read 0, 15 and 30 psia at those three points. To check that the ratio cancels supply drift, load pin 10 a little (for example 220 Ω to ground, about 23 mA). Sensor volts and SENS_5V should drop together while the ratio stays put.

## 5. C58 switch (pins 21/22)
| Condition | C58_SW ADC V | Display |
|---|---|---|
| pins 21-22 shorted (switch closed) | about 0 V | closed |
| open | about 2.7 V | OPEN (restricted or broken wire) |

With the real switch, it should read closed at rest. Optional: pull 5 inHg on its port and it should open.

## 6. Shunt amplifier (pins 5/6)
1. Short pins 5 and 6 together and to pin 3. SHUNT_I should read 1.650 V ±0.01. Engine-off zero then sets it to exactly 0 A.
2. With a millivolt source (or a 10 Ω / 10 kΩ divider from a 1.5 V cell, about 1.5 mV) across pins 6 (+) and 5 (−), the board reads about +1.5 A (1 A per mV). Reverse it and the reading goes negative.
3. On the truck, if cranking shows negative amps, press **Flip sign** on the Calibrate page.

## 7. J1708 receive (pins 33/34)
1. Connect to the truck's J1587 pair (diag connector C14 F/G, or the C55 harness), or to a USB-J1708 adapter sending test frames.
2. J1587 tab: good frames climb, bad checksums stay near 0, and D6 flickers.
3. Key on: the engine ECM (MID 128) should appear. Check the decoded values (coolant °F, battery V, rpm 0) against the gauges.
4. Download the capture and keep it. It's the raw material for decoding the MIC↔TIM messages later.
5. **Transmit check:** scope pin 33/34 while the TIM is the only thing connected. You should see no driven traffic, ever. The DE pin (IO17) measures 0 V.

## 8. Engine-off calibration (on the truck, engine off, key on)
1. The Calibrate page header should say "Engine off" (engine speed 0 on J1587). Zeroing isn't blocked any more, so this check is up to you.
2. Record each sensor's ratio **before** zeroing:
   - C59 should be about 0.49, which confirms the 0-30 psia, 0.5-4.5 V assumption.
   - For the oil and air sensors, about 0.10 means a gauge sensor reading zero. About 0.49 means an absolute sensor; don't zero it, and tell me the reading.
3. Press **Zero all**. Any item it refuses says why.
4. Start the engine. The header changes to running (engine speed on J1587 over 300 rpm).

## 9. ESP-NOW
With the BMS powered, the Modules tab lists the BMS MAC with type `status`/`board` and its packets counting up. The BMS doesn't need to change.

Record results in a copy of this file, or just tell me the readings that don't match.
