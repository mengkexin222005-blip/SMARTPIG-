# SMARTPIG ESP32-S3 firmware (1-3 fattening pigs) - website (Firebase) version

## Files (keep in one folder named `SMARTPIG_ESP32`)
| File | Purpose |
|---|---|
| `SMARTPIG_ESP32.ino` | main program |
| `config.h` | Wi-Fi, Firebase device login, pins, feeding limits (edit this). **Contains passwords: never upload it to GitHub** |
| `cloud.h` | link to the SmartPig website through Firebase (runs in the background) |
| `feed_table.h` | daily feed per pig by age (B-MEG chart, 36-140 days) |
| `RandomForestRegressor.h` | trained Random Forest (made by `train_export_rf.py`) |
| `rf_test_vectors.h` | test inputs + Python's answers for the `rftest` command (made by `train_export_rf.py`) |
| `train_export_rf.py` + `SMARTPIG_8_Adaptive_Dataset.csv` | retrain (all pig counts) and re-export the forest and test values |

## Arduino IDE setup
1. Install the **esp32 by Espressif** board package, pick **ESP32S3 Dev Module**, 32 MB flash, **OPI PSRAM** (check the N32R16V datasheet if it bootloops).
2. Library Manager: **HX711** (Bogdan Necula), **RTClib** (Adafruit), **LiquidCrystal I2C** (Frank de Brabander), **ArduinoJson 7.x**.
3. Edit `config.h` (Wi-Fi name, password, server URL), then upload. Serial Monitor: 115200 baud.

## Wiring (your connections)
| Part | Pin | ESP32-S3 |
|---|---|---|
| SMS2113 via TTL converter | TTL TXD | GPIO5 (ESP RX) |
| | TTL RXD | GPIO4 (ESP TX) |
| | VCC / GND | 5 V buck / GND |
| BTS7960 | RPWM / LPWM | GPIO6 / GPIO7 |
| | R_EN, L_EN, VCC | 5 V buck |
| | GND | buck GND, also ESP32 GND |
| LCD 20x4 + DS3231 | SDA / SCL | GPIO8 / GPIO9 (shared) |
| Tray HX711 | DT / SCK | GPIO10 / GPIO11 (VCC 3V3) |
| Hopper HX711 (4 cells, junction box) | DT / SCK | GPIO18 / GPIO17 (VCC 3V3) |
| P45 float switch | one wire / other | GPIO12 / GND |
All grounds are common. The motor supply ground must also join the buck/ESP32 ground.

## First-time calibration (Serial Monitor)
1. Empty hopper: `tare hopper`. Put a known weight on the hopper: `cal hopper 1600` (grams). The code's factor is 13.2692 counts/g (a 1.6 kg test weight read 1.33 kg at 15.9629); it is applied once after upload, and a later `cal hopper` is kept.
2. Empty tray: `tare tray`. Put a known weight (e.g. 500 g): `cal tray 500`.
3. `motor 3` runs the motor 3 s to test. `feed` runs one full meal now.
4. `settime 2026-11-04 21:00:00` sets the DS3231 (it also sets itself from the PC time if it lost power).
Values are saved in flash and survive power loss.

## How one meal works
1. Read the tray (live) = leftover from the last meal.
2. After 10 meals (5 days): all 10 empty (<=3 %) = +5 % feed; all 10 with leftover (>5 %) = -5 %; otherwise unchanged. Limit +-20 %, inside the B-MEG stage range.
3. Meal target = daily feed per pig (by age) x adjustment x pig count (from the web app, 1-3) x AM/PM share.
4. Random Forest (12 inputs, the last one is the pig count) predicts the amount to dispense. If it differs from `target - tray` by more than 15 % of the meal, the formula is used instead.
5. Motor runs until the hopper load cells show the drop; slows near the end; stops on jam (15 s without weight change), 4 min timeout, or low hopper.
6. Tray reading after settling + hopper drop are posted to the server.

## Website link (Firebase)
The ESP32 signs in to Firebase with `DEVICE_EMAIL` / `DEVICE_PASSWORD` from `config.h`. This must be the
device account the database rules trust (UID `vP6dp...`), never a person's website account.
`SERVER_BASE_URL`, `REPORT_INTERVAL_MS` and `CONFIG_INTERVAL_MS` in `config.h` are not used by this version.

| Website | ESP32 |
|---|---|
| Register pigs (age, 1-3 pigs) | pig count, age on the registration date (a new batch restarts the adjustment) |
| AM / PM time and on/off | the two meal times |
| Finish (pigs sold) | scheduled meals stop until new pigs are registered |
| Feed now | dispenses the asked grams, reports running / done / failed |
| Advanced settings | AI feeding on/off, Max per feeding, Manual cooldown |

It sends live readings twice a second (real time), a history record every 3 min, and every feeding (target, tray before/after,
Random Forest and formula amounts, which one decided, adjustment %, dispensed, status) to the Feeding log.
Settings are saved on the ESP32, so feeding keeps working without internet.

A working start in the Serial Monitor:
```
[net] Wi-Fi connected. IP: 192.168.x.x
[net] Firebase sign-in OK. Device UID: vP6dp...
[net] Settings updated: pigs registered, AM 07:00 on, PM 17:00 on, AI on
```
Type `status` to see the readings plus the Firebase state.

## Notes
- The LCD is 20x4 (your test sketches used 16x2). Use `LiquidCrystal_I2C lcd(addr, 16, 2)` only if your screen is 16x2.
- Water level only shows on the LCD and in the web data; it does not stop feeding.

## Checking the ESP32 against Python
1. Run `python train_export_rf.py`. It writes `RandomForestRegressor.h`, `rf_test_vectors.h` and `rf_test_vectors.csv` (36 rows: 24 from the dataset, 12 random, all pig counts).
2. Copy both `.h` files next to the sketch and upload.
3. In the Serial Monitor type `rftest`. Every row prints Python's value, the ESP32's value and the difference; the last line says `RF SELFTEST PASS` if all rows agree within 0.01 g.
Re-run steps 1-3 every time you retrain. Bench tip: `pigs 1`, `pigs 2`, `pigs 3` set the pig count by hand (the web app value replaces it at the next config fetch).
