# SmartPig feeding protocol (web ↔ ESP32)

This is the contract between the dashboard and the feeder firmware for
scheduled feeding, manual feeding, the feeding log and the thesis test data.
The dashboard is already built against it; the firmware needs to follow it.

All paths are in the Firebase Realtime Database. The ESP32 keeps signing in
with its device account (the UID the rules call the device). Times written by
the ESP32 to Firebase use `{".sv": "timestamp"}` (server time) unless noted.

---

## 1. Settings and schedule — `smartpigConfig` (ESP32 reads)

```json
{
  "schedule": {
    "s_lq3x9a": { "time": "06:30", "grams": 500, "enabled": true },
    "s_lq3xb1": { "time": "16:00", "grams": 500, "enabled": true }
  },
  "settings": {
    "maxGramsPerFeed": 1500,
    "gramsPerSecond": 20,
    "motorTimeoutSec": 90,
    "manualCooldownMin": 10,
    "observationMin": 30,
    "aiEnabled": false,
    "aiStepPct": 5,
    "aiMinGrams": 300,
    "aiMaxGrams": 800,
    "hopperLowGrams": 2000,
    "heatAlertC": 30,
    "dispenseToleranceG": 5,
    "timingToleranceSec": 60
  },
  "updatedAt": 1793418784226,
  "deviceAck": 1793418784226
}
```

ESP32:
1. About once a minute, `GET /smartpigConfig.json`.
2. If `updatedAt` changed: save `schedule` and `settings` to flash
   (`Preferences`), then `PUT /smartpigConfig/deviceAck.json` with the same
   `updatedAt` number. The dashboard shows "The feeder has the latest schedule"
   when `deviceAck === updatedAt`.
3. If Wi-Fi/LoRa is down, keep using the copy in flash. **Feeding never depends
   on the network.**

Slot times are Philippine time, `HH:MM`, compared against the DS3231.

## 2. Scheduled feeding (runs on the ESP32)

Every loop:
- For each enabled slot, if the RTC time is `HH:MM` of the slot **and**
  this slot has not been fed today, feed `grams` (or the AI's `nextGrams`
  for that slot if `aiEnabled`).
- Remember "last fed: slotId + date" in flash so a reboot can't feed twice.
- Never start while another feeding is running.
- Cap every feeding at `maxGramsPerFeed`.

## 3. Manual feeding — `smartpigCommands/feedNow`

The dashboard writes:

```json
{ "id": "c_lq3y2k", "grams": 300, "by": "<uid>", "requestedAt": 1793418784226, "status": "pending" }
```

ESP32, every ~5 s (with the normal upload):
1. `GET /smartpigCommands/feedNow.json`.
2. Act only if `status == "pending"`, `id` differs from the last handled id,
   and the request is less than **2 minutes** old (compare `requestedAt` with
   the server time; the dashboard marks older ones as "didn't respond").
3. `PATCH` `{ "status": "running", "startedAt": {".sv":"timestamp"} }`.
4. Feed. Then `PATCH`
   `{ "status": "done", "dispensedGrams": 297, "finishedAt": {".sv":"timestamp"} }`
   or `{ "status": "failed", "error": "motor timeout" }`.

The dashboard may set `status` to `"cancelled"` while it is still pending —
ignore cancelled commands.

## 4. Dispensing

- **Now (no load cells):** run the motor for `grams / gramsPerSecond` seconds.
  Measure `gramsPerSecond` once: run 30 s, weigh, divide. Log `"method": "timed"`.
- **With the hopper load cell:** run until the hopper weight drops by the
  target, or `motorTimeoutSec` passes. Log `"method": "loadcell"`.
- Motor timeout → stop, log `ok: false, error: "motor timeout"`.

## 5. Feeding log — `smartpigFeedLog` (ESP32 writes)

`POST /smartpigFeedLog.json` once per feeding:

```json
{
  "source": "schedule",
  "slot": "s_lq3x9a",
  "scheduledTime": "06:30",
  "rtcStart": "06:30:02",
  "startedAt": {".sv": "timestamp"},
  "targetGrams": 500,
  "dispensedGrams": 497.5,
  "durationMs": 24870,
  "method": "loadcell",
  "ok": true
}
```

For manual feedings use `"source": "manual"`, add `"commandId"`, and leave
out `slot`/`scheduledTime`.

After the observation period (`observationMin`), `PATCH` the same record
(use the push id returned by the POST) with the tray reading and the AI result:

```json
{ "leftoverGrams": 42.0, "nextGrams": 475 }
```

An approved user types the digital-scale weight into `scaleGrams` from the dashboard
(Table 4.2); the ESP32 never writes it.

### AI feeding (SMARTPIG_ESP32 firmware)
Each scheduled meal, in this order:
```
tray     = tray load cell right before the meal  (= leftover of the last meal, patched into its record as leftoverGrams)
5-day rule (10 meals): all 10 empty (<= 3 % of target) -> adjust +aiStepPct
                       all 10 with leftover (> 5 %)    -> adjust -aiStepPct     (limit +-20 %, inside the B-MEG stage)
target   = B-MEG g/pig/day (adjusted) x pigs x 0.5
formula  = target - tray
AI       = Random Forest (ages 36-140; inputs scaled to 3 pigs)
grams    = AI, unless it differs from the formula by > 15 % of the target -> formula
```
Extra feed log fields: `mealTargetGrams`, `trayBeforeGrams`, `trayAfterGrams`, `aiGrams`, `formulaGrams`,
`decidedBy` (`ai` | `formula`), `adjustPct`, `trend`, `ageDays`, `pigs`, `emptyMeals`, `leftoverMeals`.
Live data also carries `ageDays`, `pigs`, `adjustPct` and `state`. `aiEnabled` off = formula only, no adjustment.

## 6. Extra fields on `smartpig` and `smartpigHistory` (thesis parameters)

Add these to every upload. The database rules already allow them.

| Field | Type | Used for |
|---|---|---|
| `seq` | number, +1 each upload, reset on boot | Data Transmission: lost readings = gaps |
| `bootCount` | number, +1 on every boot (store in flash) | Stability: restarts; groups `seq` |
| `sentAt` | epoch ms from NTP at send time (omit if no NTP) | Data Transmission: delay |
| `uptimeSec` | `millis()/1000` | Stability: longest uptime |
| `freeHeap` | `ESP.getFreeHeap()` | Stability: memory |
| `resetReason` | e.g. `"POWERON"`, `"PANIC"`, `"BROWNOUT"` | Stability: why it restarted |
| `hopperGrams` | number (when the load cell works) | Overview + System |
| `trayGrams` | number (when the load cell works) | Overview + System |

## 7. Permissions (already in `database.rules.json`)

Anyone can view. Everyone who signs up is a Farmer (`role: "user"`) with full
access; `disabled` = access removed. Older `admin` / `pending` roles count as full access.

| Path | Visitor / disabled | Farmer | ESP32 |
|---|---|---|---|
| `smartpig`, `smartpigHistory` | read | read | write |
| `smartpigConfig/schedule` (`am`, `pm`: `time`, `enabled`) | read | write | read |
| `smartpigConfig/pen` (`count` 1–3, `ageDays` 1–180, `registeredDate`) | read | create when empty, or clear (Finish) | read |
| `smartpigConfig/settings`, `updatedAt` | read | write | read |
| `smartpigConfig/deviceAck` | read | read | write |
| `smartpigBatches/*` (finished batches) | read | add | — |
| `smartpigCommands/feedNow` | read | send / cancel | set running / done / failed |
| `smartpigFeedLog/*` | read | read; write `scaleGrams` | write |
| `users/*` | own profile | read all; remove / restore others | — |

### Scheduled amount (registered pigs)

`age today = ageDays + days since registeredDate` (Philippine date, from the DS3231).
`grams per feeding = g per pig per day × count ÷ 2` (then the AI step above), capped at `maxGramsPerFeed`.
No pigs registered = no scheduled feedings (Feed now still works).

| Age (days) | g per pig per day |
|---|---|
| 1–35 | 500 (group table) |
| 36–140 | B-MEG Premium chart, one value per day (250 → 2500), `feedingData.js` / `feed_table.h` |
| 141–180 (and older) | 2500 (group table) |
