# SMARTPIG ESP32-S3 firmware (website version)

Your groupmate's feeder (B-MEG chart, 5-day leftover rule, Random Forest, hopper-weight
dispensing) connected to the SMARTPIG website through Firebase.

## Files (keep them in one folder named `SMARTPIG_ESP32`)
| File | Purpose |
|---|---|
| `SMARTPIG_ESP32.ino` | main program: sensors, LCD, schedule, AI feeding, dispensing |
| `cloud.h` | Wi-Fi + Firebase (website link), runs in the background on core 0 |
| `secrets.h` | Wi-Fi name/password and the Firebase device login. **Private: never upload to GitHub** |
| `secrets.example.h` | empty copy of `secrets.h` for groupmates |
| `config.h` | pins and the fixed parts of the feeding rule |
| `feed_table.h` | feed per pig per day: 1-35 days 500 g, 36-140 B-MEG chart, 141+ 2500 g |
| `RandomForestRegressor.h` | trained Random Forest (made by `train_export_rf.py`) |
| `train_export_rf.py` + `SMARTPIG_8_Adaptive_Dataset.csv` | retrain and re-export the forest |

## Arduino IDE setup
1. Board package **esp32 by Espressif**, board **ESP32S3 Dev Module**, 32 MB flash, **OPI PSRAM**, USB CDC On Boot: Enabled.
2. Library Manager: **HX711** (Bogdan Necula), **RTClib** (Adafruit), **LiquidCrystal I2C** (Frank de Brabander), **ArduinoJson 7.x**.
3. Check `secrets.h` (2.4 GHz Wi-Fi; the device account the database rules trust), then upload. Serial Monitor: 115200 baud.

A working start looks like:
```
[net] Wi-Fi connected. IP: 192.168.x.x
[net] Firebase sign-in OK. Device UID: vP6d...
[net] Settings updated: pigs registered, AM 07:00 on, PM 17:00 on, AI on
[net] Schedule synced (deviceAck).
```
The Device UID must match the one in `database.rules.json`, or Firebase refuses the data (HTTP 401/403).

## What the website controls
| Website | Feeder |
|---|---|
| Register pigs (age, 1-3 pigs) | age counts up every day; new pigs restart the AI adjustment |
| AM / PM time and on/off | scheduled meals (up to 30 min late if the ESP32 was off) |
| Feed now | dispenses the asked grams, reports running / done / failed |
| Finish (pigs sold) | scheduled meals stop until new pigs are registered |
| Advanced settings | Max per feeding, Motor speed + timeout (safety stop), Manual cooldown, AI feeding on/off, AI step |

## How one scheduled meal works
1. Read the tray = leftover from the last meal (sent to the website as that meal's Leftover).
2. After 10 meals (5 days): all 10 empty (<= 3 %) = +AI step; all 10 with leftover (> 5 %) = -AI step. Limit +-20 %, inside the B-MEG stage.
3. Meal target = feed per pig per day (by age, adjusted) x pigs x 0.5.
4. Random Forest predicts the amount (ages 36-140; inputs scaled to 3 pigs). If it differs from `target - tray` by more than 15 % of the target, the formula is used. The website log shows which one decided.
5. Motor runs until the hopper load cells show the drop; slows near the end; stops on jam (15 s without weight change), the safety time, or low hopper.

## Serial commands
`status` | `tare hopper` | `cal hopper 5000` | `tare tray` | `cal tray 500` | `feed 300` | `motor 3` | `settime 2026-11-04 21:00:00` | `reset adjust`

Calibration is saved on the ESP32 and kept when you upload this version over your groupmate's.

## Wiring
| Part | ESP32-S3 |
|---|---|
| SMS2113 via TTL RS485 | TXD -> GPIO5, RXD -> GPIO4 |
| BTS7960 | RPWM GPIO6, LPWM GPIO7, R_EN + L_EN to 5 V |
| LCD 20x4 + DS3231 | SDA GPIO8, SCL GPIO9 |
| Tray HX711 | DT GPIO10, SCK GPIO11 |
| Hopper HX711 (junction box) | DT GPIO18, SCK GPIO17 |
| P45 float switch | GPIO12 and GND |
