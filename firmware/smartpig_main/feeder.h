#pragma once

// SmartPig feeder: motor, schedule, manual "Feed now", feeding log, leftover check and AI amount.
// Runs in the main loop (core 1). The network task (firebase_uploader.h)
// downloads the schedule, hands over manual commands and uploads results.
// Contract: web/docs/feeding-protocol.md

#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#include <sys/time.h>
#include "scales.h"

// ---------------- Motor driver BTS7960 (as actually wired) ----------------
#define MOTOR_RPWM_PIN 6    // BTS7960 RPWM (forward)
#define MOTOR_LPWM_PIN 7    // BTS7960 LPWM (reverse, kept at 0)
#define MOTOR_REN_PIN  -1   // R_EN tied to 5V (always enabled). Set a GPIO here if you wire it to the ESP32.
#define MOTOR_LEN_PIN  -1   // L_EN tied to 5V (always enabled)
#define MOTOR_SPEED    180  // 0–255. Calibrate gramsPerSecond at the speed you use.
// If the auger turns the wrong way, swap the motor wires M+ and M- on the driver.

// ---------------- Behaviour ----------------
#define MAX_SLOTS 8
#define SCHEDULE_GRACE_MIN 10        // a feeding may start up to 10 min late (e.g. after a power cut)
#define COMMAND_TTL_MS 120000ULL     // ignore manual commands older than 2 minutes
#define LOG_QUEUE_SIZE 8
#define SETTLE_MS 3000UL             // wait after the motor stops before weighing

// ---------------- Registered pigs -> feeding amount ----------------
// Same table as the website (feedingData.js). kg of feed per pig per day by age.
// Older than the last row = the last row's amount. Two feedings a day (AM + PM).
struct FeedRow { uint16_t from; uint16_t to; float kg; };
static const FeedRow FEED_TABLE[] = {
  {1, 60, 0.5f},
  {61, 90, 1.2f},
  {91, 120, 1.6f},
  {121, 150, 2.1f},
  {151, 180, 2.5f},
};
static const uint8_t FEED_ROWS = sizeof(FEED_TABLE) / sizeof(FEED_TABLE[0]);
#define FEEDS_PER_DAY 2

static float feedKgPerPig(int ageDays) {
  for (uint8_t i = 0; i < FEED_ROWS; i++) {
    if (ageDays >= FEED_TABLE[i].from && ageDays <= FEED_TABLE[i].to) return FEED_TABLE[i].kg;
  }
  return ageDays > FEED_TABLE[FEED_ROWS - 1].to ? FEED_TABLE[FEED_ROWS - 1].kg : FEED_TABLE[0].kg;
}

struct FeedSlot {
  char id[32];
  uint8_t hour;
  uint8_t minute;
  uint32_t grams;
  bool enabled;
};

struct FeedConfig {
  FeedSlot slots[MAX_SLOTS];
  uint8_t slotCount;
  float maxGramsPerFeed;
  float gramsPerSecond;
  uint32_t motorTimeoutSec;
  uint32_t manualCooldownMin;
  uint32_t observationMin;
  bool aiEnabled;
  float aiStepPct;
  float aiMinGrams;
  float aiMaxGrams;
  // Registered pigs (smartpigConfig/pen). hasPen = false -> no scheduled feeding.
  bool hasPen;
  uint8_t penCount;
  uint16_t penAgeDays;     // age when registered
  uint16_t penYear;        // registration date (Philippine time)
  uint8_t penMonth;
  uint8_t penDay;
  uint64_t updatedAt;
};

// One finished feeding, waiting to be uploaded to /smartpigFeedLog
struct FeedRecord {
  uint32_t localId;        // links the later leftover/AI update to this record
  char source[10];         // "schedule" | "manual"
  char slot[32];
  char commandId[32];
  char scheduledTime[6];   // "HH:MM"
  char rtcStart[9];        // "HH:MM:SS"
  uint64_t startedAtMs;    // NTP epoch ms, 0 = unknown (server time is used)
  float targetGrams;
  float dispensedGrams;
  uint32_t durationMs;
  bool loadcell;           // method "loadcell" or "timed"
  bool ok;
  char error[40];
};

// Leftover in the tray after the observation period (+ AI next amount)
struct ObsResult {
  uint32_t localId;
  float leftoverGrams;
  float nextGrams;         // 0 = no AI result
};

struct ManualCommand {
  bool pending;
  char id[32];
  float grams;
};

// Latest status of the manual command, for the network task to PATCH
struct CommandResult {
  bool ready;
  char id[32];
  char status[10];         // "running" | "done" | "failed"
  float dispensedGrams;
  char error[40];
};

static FeedConfig feedCfg;
static SemaphoreHandle_t feedMutex = nullptr;
static Preferences feedPrefs;

static FeedRecord logQueue[LOG_QUEUE_SIZE];
static uint8_t logCount = 0;
static ObsResult obsQueue[LOG_QUEUE_SIZE];
static uint8_t obsCount = 0;
static ManualCommand manualCmd = {};
static CommandResult cmdResult = {};

static uint32_t feederBootCount = 0;
static uint32_t nextLocalId = 1;

// Running feeding (main loop only)
enum FeedState { FEED_IDLE, FEED_RUNNING, FEED_SETTLING };
static FeedState feedState = FEED_IDLE;
static unsigned long feedStartMs = 0;
static unsigned long feedPlannedMs = 0;
static unsigned long feedLimitMs = 0;
static unsigned long motorStopMs = 0;
static bool useLoadcell = false;
static float hopperStart = 0;
static FeedRecord current = {};
static bool currentIsManual = false;
static bool haveLastManual = false;
static unsigned long lastManualEndMs = 0;

// Waiting for the observation period to end (main loop only)
static bool obsPending = false;
static unsigned long obsDueMs = 0;
static uint32_t obsLocalId = 0;
static float obsDispensed = 0;
static float obsTarget = 0;
static char obsSlot[32] = "";
static bool obsUseAi = false;

// ---------------- Time helpers ----------------

// Real time from NTP (set by the network task). 0 until synced.
uint64_t epochMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1700000000) return 0;
  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

static uint32_t slotHash(const char *s) {
  uint32_t h = 5381;
  while (*s) h = ((h << 5) + h) + (uint8_t)(*s++);
  return h;
}

static void slotKey(char *out, size_t n, const char *prefix, const char *slotId) {
  snprintf(out, n, "%s%08lx", prefix, (unsigned long)slotHash(slotId));
}

// ---------------- Config ----------------

static void feederDefaults(FeedConfig &c) {
  memset(&c, 0, sizeof(c));
  c.maxGramsPerFeed = 4000;
  c.gramsPerSecond = 20;
  c.motorTimeoutSec = 90;
  c.manualCooldownMin = 10;
  c.observationMin = 30;
  c.aiEnabled = false;
  c.aiStepPct = 5;
  c.aiMinGrams = 300;
  c.aiMaxGrams = 800;
}

// Parses the JSON of /smartpigConfig into a FeedConfig
static bool feederParseConfig(const String &json, FeedConfig &out) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;

  feederDefaults(out);

  JsonObject settings = doc["settings"];
  if (!settings.isNull()) {
    out.maxGramsPerFeed = settings["maxGramsPerFeed"] | out.maxGramsPerFeed;
    out.gramsPerSecond = settings["gramsPerSecond"] | out.gramsPerSecond;
    out.motorTimeoutSec = settings["motorTimeoutSec"] | out.motorTimeoutSec;
    out.manualCooldownMin = settings["manualCooldownMin"] | out.manualCooldownMin;
    out.observationMin = settings["observationMin"] | out.observationMin;
    out.aiEnabled = settings["aiEnabled"] | out.aiEnabled;
    out.aiStepPct = settings["aiStepPct"] | out.aiStepPct;
    out.aiMinGrams = settings["aiMinGrams"] | out.aiMinGrams;
    out.aiMaxGrams = settings["aiMaxGrams"] | out.aiMaxGrams;
  }
  if (out.gramsPerSecond <= 0) out.gramsPerSecond = 20;
  if (out.motorTimeoutSec == 0) out.motorTimeoutSec = 90;

  // Registered pigs
  JsonObject pen = doc["pen"];
  if (!pen.isNull()) {
    int count = pen["count"] | 0;
    int age = pen["ageDays"] | 0;
    const char *date = pen["registeredDate"] | "";
    int y = 0, mo = 0, d = 0;
    if (count >= 1 && count <= 3 && age > 0 && sscanf(date, "%d-%d-%d", &y, &mo, &d) == 3) {
      out.hasPen = true;
      out.penCount = count;
      out.penAgeDays = age;
      out.penYear = y;
      out.penMonth = mo;
      out.penDay = d;
    }
  }

  // Two fixed times: "am" and "pm" (older free-form slots are ignored)
  JsonObject schedule = doc["schedule"];
  if (!schedule.isNull()) {
    for (JsonPair p : schedule) {
      if (out.slotCount >= MAX_SLOTS) break;
      if (strcmp(p.key().c_str(), "am") != 0 && strcmp(p.key().c_str(), "pm") != 0) continue;
      const char *t = p.value()["time"] | "";
      int h = -1, m = -1;
      if (sscanf(t, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) continue;
      FeedSlot &s = out.slots[out.slotCount++];
      strlcpy(s.id, p.key().c_str(), sizeof(s.id));
      s.hour = h;
      s.minute = m;
      s.grams = p.value()["grams"] | 0;
      s.enabled = p.value()["enabled"] | true;
    }
  }

  out.updatedAt = doc["updatedAt"] | (uint64_t)0;
  return true;
}

// Called by the network task when a new config arrives
void feederApplyConfig(const String &json) {
  FeedConfig next;
  if (!feederParseConfig(json, next)) {
    Serial.println("[feed] Config JSON not valid, ignored.");
    return;
  }
  if (xSemaphoreTake(feedMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    feedCfg = next;
    xSemaphoreGive(feedMutex);
  }
  feedPrefs.putString("cfg", json);   // keep a copy so feeding works offline
  Serial.printf("[feed] Schedule updated: %d time(s), %s.\n", next.slotCount,
                next.hasPen ? "pigs registered" : "no pigs registered");
}

uint64_t feederConfigVersion() {
  uint64_t v = 0;
  if (xSemaphoreTake(feedMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    v = feedCfg.updatedAt;
    xSemaphoreGive(feedMutex);
  }
  return v;
}

// ---------------- Motor ----------------

// Enable pins are optional: -1 means "tied to 5V", so the code leaves them alone
static void motorEnable(bool on) {
  if (MOTOR_REN_PIN >= 0) digitalWrite(MOTOR_REN_PIN, on ? HIGH : LOW);
  if (MOTOR_LEN_PIN >= 0) digitalWrite(MOTOR_LEN_PIN, on ? HIGH : LOW);
}

static void motorOff() {
  analogWrite(MOTOR_RPWM_PIN, 0);
  analogWrite(MOTOR_LPWM_PIN, 0);
  motorEnable(false);
}

static void motorOn() {
  analogWrite(MOTOR_LPWM_PIN, 0);
  motorEnable(true);
  analogWrite(MOTOR_RPWM_PIN, MOTOR_SPEED);
}

// ---------------- Setup ----------------

void feederBegin() {
  // Motor off first, before anything else can go wrong
  pinMode(MOTOR_RPWM_PIN, OUTPUT);
  pinMode(MOTOR_LPWM_PIN, OUTPUT);
  if (MOTOR_REN_PIN >= 0) pinMode(MOTOR_REN_PIN, OUTPUT);
  if (MOTOR_LEN_PIN >= 0) pinMode(MOTOR_LEN_PIN, OUTPUT);
  motorOff();

  feedMutex = xSemaphoreCreateMutex();
  feedPrefs.begin("smartpig", false);

  feederBootCount = feedPrefs.getUInt("boot", 0) + 1;
  feedPrefs.putUInt("boot", feederBootCount);
  nextLocalId = feederBootCount * 1000UL;   // unique enough across reboots

  feederDefaults(feedCfg);
  String saved = feedPrefs.getString("cfg", "");
  if (saved.length() > 0 && feederParseConfig(saved, feedCfg)) {
    Serial.printf("[feed] Loaded saved schedule: %d time(s).\n", feedCfg.slotCount);
  } else {
    Serial.println("[feed] No saved schedule yet.");
  }
}

// ---------------- Helpers for the network task ----------------

void feederSetManualCommand(const char *id, float grams) {
  if (xSemaphoreTake(feedMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    manualCmd.pending = true;
    strlcpy(manualCmd.id, id, sizeof(manualCmd.id));
    manualCmd.grams = grams;
    xSemaphoreGive(feedMutex);
  }
}

bool feederTakeCommandResult(CommandResult &out) {
  bool got = false;
  if (xSemaphoreTake(feedMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (cmdResult.ready) {
      out = cmdResult;
      cmdResult.ready = false;
      got = true;
    }
    xSemaphoreGive(feedMutex);
  }
  return got;
}

void feederRequeueCommandResult(const CommandResult &r) {
  if (xSemaphoreTake(feedMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (!cmdResult.ready) {   // don't overwrite a newer status
      cmdResult = r;
      cmdResult.ready = true;
    }
    xSemaphoreGive(feedMutex);
  }
}

template <typename T>
static bool peekQueue(T *queue, uint8_t &count, T &out) {
  bool got = false;
  if (xSemaphoreTake(feedMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (count > 0) {
      out = queue[0];
      got = true;
    }
    xSemaphoreGive(feedMutex);
  }
  return got;
}

template <typename T>
static void popQueue(T *queue, uint8_t &count) {
  if (xSemaphoreTake(feedMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (count > 0) {
      for (uint8_t i = 1; i < count; i++) queue[i - 1] = queue[i];
      count--;
    }
    xSemaphoreGive(feedMutex);
  }
}

template <typename T>
static void pushQueue(T *queue, uint8_t &count, const T &item) {
  if (xSemaphoreTake(feedMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (count >= LOG_QUEUE_SIZE) {       // full: drop the oldest
      for (uint8_t i = 1; i < count; i++) queue[i - 1] = queue[i];
      count--;
    }
    queue[count++] = item;
    xSemaphoreGive(feedMutex);
  }
}

bool feederPeekLog(FeedRecord &out) { return peekQueue(logQueue, logCount, out); }
void feederPopLog() { popQueue(logQueue, logCount); }
bool feederPeekObs(ObsResult &out) { return peekQueue(obsQueue, obsCount, out); }
void feederPopObs() { popQueue(obsQueue, obsCount); }

// ---------------- Main-loop side ----------------

static void setCommandStatus(const char *id, const char *status, float grams, const char *error) {
  if (xSemaphoreTake(feedMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    cmdResult.ready = true;
    strlcpy(cmdResult.id, id, sizeof(cmdResult.id));
    strlcpy(cmdResult.status, status, sizeof(cmdResult.status));
    cmdResult.dispensedGrams = grams;
    strlcpy(cmdResult.error, error ? error : "", sizeof(cmdResult.error));
    xSemaphoreGive(feedMutex);
  }
}

// AI amount for a slot (0 = none saved yet)
static float slotAiGrams(const char *slotId) {
  char key[16];
  slotKey(key, sizeof(key), "ng", slotId);
  return feedPrefs.getFloat(key, 0);
}

// Weighs the tray after the observation period and works out the next amount
static void finishObservation(const FeedConfig &cfg) {
  obsPending = false;
  float leftover;
  if (!trayGrams(leftover)) {
    Serial.println("[feed] Tray scale not ready, leftover not recorded.");
    return;
  }
  if (leftover < 0) leftover = 0;

  ObsResult r = {};
  r.localId = obsLocalId;
  r.leftoverGrams = leftover;

  if (obsUseAi && obsDispensed > 0) {
    float waste = leftover / obsDispensed;
    float next = obsTarget;
    if (waste > 0.10f) next = obsTarget * (1 - cfg.aiStepPct / 100.0f);
    else if (waste < 0.02f) next = obsTarget * (1 + cfg.aiStepPct / 100.0f);
    next = constrain(next, cfg.aiMinGrams, cfg.aiMaxGrams);
    next = roundf(next);
    r.nextGrams = next;

    char key[16];
    slotKey(key, sizeof(key), "ng", obsSlot);
    feedPrefs.putFloat(key, next);
    Serial.printf("[feed] Leftover %.0f g (%.1f%%) -> next %.0f g\n", leftover, waste * 100, next);
  } else {
    Serial.printf("[feed] Leftover %.0f g\n", leftover);
  }
  pushQueue(obsQueue, obsCount, r);
}

static void startFeed(float grams, bool manual, const FeedSlot *slot, const char *cmdId,
                      const DateTime &now, bool rtcValid, const FeedConfig &cfg) {
  // A new feeding ends the previous observation early (weigh the tray now)
  if (obsPending) finishObservation(cfg);

  memset(&current, 0, sizeof(current));
  current.localId = nextLocalId++;
  strlcpy(current.source, manual ? "manual" : "schedule", sizeof(current.source));
  if (slot) {
    strlcpy(current.slot, slot->id, sizeof(current.slot));
    snprintf(current.scheduledTime, sizeof(current.scheduledTime), "%02d:%02d", slot->hour, slot->minute);
  }
  if (cmdId) strlcpy(current.commandId, cmdId, sizeof(current.commandId));
  if (rtcValid) {
    snprintf(current.rtcStart, sizeof(current.rtcStart), "%02d:%02d:%02d",
             now.hour(), now.minute(), now.second());
  }
  current.startedAtMs = epochMs();

  if (cfg.maxGramsPerFeed > 0 && grams > cfg.maxGramsPerFeed) grams = cfg.maxGramsPerFeed;
  current.targetGrams = grams;

  // Use the hopper load cell when it works, otherwise run by time
  useLoadcell = hopperGrams(hopperStart);
  current.loadcell = useLoadcell;

  feedPlannedMs = (unsigned long)((grams / cfg.gramsPerSecond) * 1000.0f);
  // Safety stop: the motor timeout, or longer for a big amount (1.5x the planned time + 10 s)
  feedLimitMs = max((unsigned long)(cfg.motorTimeoutSec * 1000UL), feedPlannedMs + feedPlannedMs / 2 + 10000UL);
  feedStartMs = millis();
  currentIsManual = manual;
  feedState = FEED_RUNNING;
  motorOn();

  Serial.printf("[feed] START %s %.0f g (%s)\n", current.source, grams, useLoadcell ? "load cell" : "timed");
  if (manual) setCommandStatus(current.commandId, "running", 0, nullptr);
}

static void stopMotor(const char *error) {
  motorOff();
  motorStopMs = millis();
  current.durationMs = motorStopMs - feedStartMs;
  current.ok = (error == nullptr);
  if (error) strlcpy(current.error, error, sizeof(current.error));
  feedState = FEED_SETTLING;
}

static void finishFeed(const FeedConfig &cfg) {
  feedState = FEED_IDLE;

  float hopperNow;
  if (useLoadcell && hopperGrams(hopperNow)) {
    current.dispensedGrams = hopperStart - hopperNow;
  } else {
    // Timed dispensing: estimate from the motor calibration
    current.loadcell = false;
    current.dispensedGrams = cfg.gramsPerSecond * (current.durationMs / 1000.0f);
  }
  if (current.dispensedGrams < 0) current.dispensedGrams = 0;

  Serial.printf("[feed] DONE %.1f g in %lu ms%s\n", current.dispensedGrams,
                (unsigned long)current.durationMs, current.ok ? "" : " (failed)");

  pushQueue(logQueue, logCount, current);
  if (currentIsManual) {
    setCommandStatus(current.commandId, current.ok ? "done" : "failed", current.dispensedGrams,
                     current.ok ? nullptr : current.error);
    haveLastManual = true;
    lastManualEndMs = millis();
  }

  // Start the observation period (leftover + AI). Needs the tray scale.
  float t;
  if (cfg.observationMin > 0 && trayGrams(t)) {
    obsPending = true;
    obsDueMs = millis() + cfg.observationMin * 60000UL;
    obsLocalId = current.localId;
    obsDispensed = current.dispensedGrams;
    obsTarget = current.targetGrams;
    strlcpy(obsSlot, current.slot, sizeof(obsSlot));
    obsUseAi = cfg.aiEnabled && !currentIsManual && current.slot[0];
  }
}

// Age of the registered pigs today, counted from the registration date
static int penAgeToday(const FeedConfig &cfg, const DateTime &now) {
  DateTime reg(cfg.penYear, cfg.penMonth, cfg.penDay);
  DateTime today(now.year(), now.month(), now.day());
  long days = ((long)today.unixtime() - (long)reg.unixtime()) / 86400L;
  if (days < 0) days = 0;
  return cfg.penAgeDays + days;
}

static float penGramsPerFeed(const FeedConfig &cfg, const DateTime &now) {
  if (!cfg.hasPen) return 0;
  return feedKgPerPig(penAgeToday(cfg, now)) * 1000.0f * cfg.penCount / FEEDS_PER_DAY;
}

bool feederIsFeeding() { return feedState != FEED_IDLE; }
float feederCurrentTarget() { return current.targetGrams; }

// Call every loop with the RTC time
void feederLoop(const DateTime &now, bool rtcValid) {
  FeedConfig cfg;
  if (xSemaphoreTake(feedMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
  cfg = feedCfg;
  ManualCommand cmd = manualCmd;
  manualCmd.pending = false;
  xSemaphoreGive(feedMutex);

  // 1. A feeding is running
  if (feedState != FEED_IDLE) {
    if (cmd.pending) setCommandStatus(cmd.id, "failed", 0, "feeder busy");

    if (feedState == FEED_RUNNING) {
      unsigned long ran = millis() - feedStartMs;
      if (ran >= feedLimitMs) {
        stopMotor("motor timeout");
      } else if (useLoadcell) {
        float h;
        if (hopperGrams(h) && hopperStart - h >= current.targetGrams) stopMotor(nullptr);
      } else if (ran >= feedPlannedMs) {
        stopMotor(nullptr);
      }
    } else if (feedState == FEED_SETTLING && millis() - motorStopMs >= SETTLE_MS) {
      finishFeed(cfg);
    }
    return;
  }

  // 2. Observation period over: weigh the leftover
  if (obsPending && (long)(millis() - obsDueMs) >= 0) finishObservation(cfg);

  // 3. Manual "Feed now"
  if (cmd.pending) {
    unsigned long cooldownMs = cfg.manualCooldownMin * 60000UL;
    if (haveLastManual && millis() - lastManualEndMs < cooldownMs) {
      setCommandStatus(cmd.id, "failed", 0, "cooldown");
    } else if (cmd.grams < 10) {
      setCommandStatus(cmd.id, "failed", 0, "amount too small");
    } else {
      startFeed(cmd.grams, true, nullptr, cmd.id, now, rtcValid, cfg);
    }
    return;
  }

  // 4. Schedule (needs a valid RTC and registered pigs)
  if (!rtcValid || !cfg.hasPen) return;
  int nowMin = now.hour() * 60 + now.minute();
  uint32_t today = now.year() * 10000UL + now.month() * 100UL + now.day();

  for (uint8_t i = 0; i < cfg.slotCount; i++) {
    const FeedSlot &s = cfg.slots[i];
    if (!s.enabled) continue;
    int slotMin = s.hour * 60 + s.minute;
    if (nowMin < slotMin || nowMin >= slotMin + SCHEDULE_GRACE_MIN) continue;

    char key[16];
    slotKey(key, sizeof(key), "lf", s.id);
    if (feedPrefs.getUInt(key, 0) == today) continue;   // already fed today

    feedPrefs.putUInt(key, today);                      // saved before starting: no double feed after a reboot

    // Amount from the registered pigs: kg per pig per day (by today's age) x pigs / 2
    float grams = penGramsPerFeed(cfg, now);
    if (grams < 10) continue;
    if (cfg.aiEnabled) {
      float ai = slotAiGrams(s.id);
      if (ai > 0) grams = ai;
    }
    startFeed(grams, false, &s, nullptr, now, rtcValid, cfg);
    return;
  }
}
