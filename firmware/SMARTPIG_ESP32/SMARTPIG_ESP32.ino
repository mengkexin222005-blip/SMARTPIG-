/*
  SMARTPIG - IoT-Based Automated Feeding and Real-Time Monitoring System
  with Rule-Based AI Precision Feeding (1-3 fattening pigs, Buhi, Camarines Sur)

  Board   : ESP32-S3-WROOM-2-N32R16V (Arduino core for ESP32)
  Libraries (Library Manager):
    - HX711 by Bogdan Necula (bogde)
    - RTClib by Adafruit
    - LiquidCrystal I2C by Frank de Brabander
    - ArduinoJson by Benoit Blanchon (version 7.x)
  Built in: WiFi, WiFiClientSecure, HTTPClient, Preferences, Wire

  Files in this folder (keep them together):
    SMARTPIG_ESP32.ino, config.h, secrets.h, feed_table.h, cloud.h, RandomForestRegressor.h

  The website controls the feeder through Firebase:
    - Register pigs (age + 1-3 pigs) -> the feeder counts the age up every day
    - AM / PM feeding times (on/off)   -> the schedule below
    - Feed now                         -> runs a manual feeding of the asked grams
    - Finish (pigs sold)               -> scheduled feedings stop until new pigs are registered
  Every feeding goes to the website's Feeding log, with the AI decision.
*/

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <RTClib.h>
#include <LiquidCrystal_I2C.h>
#include <HX711.h>

#include "config.h"
#include "feed_table.h"
#include "RandomForestRegressor.h"
#include "cloud.h"

#define PH_OFFSET_SEC (8L * 3600L)   // Philippine time = UTC + 8, no daylight saving

// ------------------------------------------------------------------
// Objects
// ------------------------------------------------------------------
HardwareSerial RS485(1);
HX711 trayScale, hopperScale;
RTC_DS3231 rtc;
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
Preferences prefs;
Eloquent::ML::Port::RandomForestRegressor rf;

// ------------------------------------------------------------------
// Saved state (survives power loss). Same layout as before, so the
// calibration already saved on the ESP32 is kept.
// ------------------------------------------------------------------
#define STATE_MAGIC 0x5350494Au  // "SPIJ"
struct State {
  uint32_t magic;
  float calTray, calHopper;      // HX711 counts per gram (set by calibration)
  long offTray, offHopper;       // HX711 zero offsets (set by tare)
  int16_t adjPct;                // current feed adjustment, -20..+20 %
  uint8_t histCount;             // number of valid leftover records
  float histLeft[HIST_N];        // leftover (g) read before the next meal
  float histTgt[HIST_N];         // the meal target (g) that leftover belongs to
  float lastTarget;              // target of the most recent scheduled meal
  int32_t lastMealKey;           // dayIndex*2 + mealIndex of the last meal done
  int16_t baseAge;               // (not used any more: the age comes from the website)
  uint32_t baseDay;              // (not used any more)
  uint8_t mealH[2], mealM[2];    // (not used any more: the times come from the website)
  float amRatio;                 // morning share of the daily feed
  bool enabled;                  // (not used any more)
} S;
uint32_t gPenSig = 0;            // which pig batch the adjustment belongs to

// ------------------------------------------------------------------
// Live values
// ------------------------------------------------------------------
float gTemp = NAN, gHum = NAN;       // last good SMS2113 reading
bool gSensorOk = false;
unsigned long gSensorOkAt = 0;
float gHopper = NAN, gTray = NAN;    // grams
float gPrevHopper = NAN;
bool gWaterOk = true;
bool gRtcOk = false;
const char *gState = "IDLE";
unsigned long gLastManualAt = 0;
bool gManualDone = false;

unsigned long tSensor = 0, tWeight = 0, tLcd = 0, tPublish = 0, tRtcSync = 0, tRtcDaily = 0;
bool gRtcSyncedOnce = false;

// ------------------------------------------------------------------
// Helpers: state storage
// ------------------------------------------------------------------
void setDefaults() {
  memset(&S, 0, sizeof(S));
  S.magic = STATE_MAGIC;
  S.calTray = DEFAULT_TRAY_CAL_PER_G;
  S.calHopper = DEFAULT_HOPPER_CAL_PER_G;
  S.lastMealKey = -1;
  S.amRatio = DEFAULT_AM_RATIO;
  S.enabled = true;
}

void saveState() { prefs.putBytes("state", &S, sizeof(S)); }

void loadState() {
  prefs.begin("smartpig", false);
  size_t n = prefs.getBytesLength("state");
  if (n == sizeof(S)) {
    prefs.getBytes("state", &S, sizeof(S));
    if (S.magic == STATE_MAGIC) { gPenSig = prefs.getUInt("penSig", 0); return; }
  }
  setDefaults();
}

// ------------------------------------------------------------------
// Weight sensors (HX711)
// ------------------------------------------------------------------
float safeWeight(HX711 &sc, int samples) {
  if (!sc.wait_ready_timeout(1000)) return NAN;   // sensor not answering
  return sc.get_units(samples);
}
float readTray(int n = 5)   { return safeWeight(trayScale, n); }
float readHopper(int n = 5) { return safeWeight(hopperScale, n); }

void applyCalibration() {
  trayScale.set_scale(S.calTray);     trayScale.set_offset(S.offTray);
  hopperScale.set_scale(S.calHopper); hopperScale.set_offset(S.offHopper);
}

// ------------------------------------------------------------------
// SMS2113 temperature / humidity (Modbus RTU, slave 1, 9600 baud, fn 03)
// register 0 = humidity, register 1 = temperature (value x 10)
// ------------------------------------------------------------------
uint16_t crc16(const uint8_t *b, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= b[i];
    for (int j = 0; j < 8; j++) c = (c & 1) ? (c >> 1) ^ 0xA001 : (c >> 1);
  }
  return c;
}

bool readSms2113(float &t, float &h) {
  uint8_t req[8] = {SMS_ADDR, 0x03, 0x00, 0x00, 0x00, 0x02, 0, 0};
  uint16_t c = crc16(req, 6);
  req[6] = c & 0xFF; req[7] = c >> 8;
  while (RS485.available()) RS485.read();
  RS485.write(req, 8);
  RS485.flush();
  uint8_t rsp[9]; size_t n = 0; unsigned long t0 = millis();
  while (n < 9 && millis() - t0 < 500) { if (RS485.available()) rsp[n++] = RS485.read(); }
  if (n < 9 || rsp[0] != SMS_ADDR || rsp[1] != 0x03 || rsp[2] != 4) return false;
  uint16_t rc = rsp[7] | (rsp[8] << 8);
  if (rc != crc16(rsp, 7)) return false;
  float hh = (int16_t)((rsp[3] << 8) | rsp[4]) / 10.0f;
  float tt = (int16_t)((rsp[5] << 8) | rsp[6]) / 10.0f;
  if (hh < 0 || hh > 100 || tt < -20 || tt > 70) return false;
  h = hh; t = tt;
  return true;
}

// ------------------------------------------------------------------
// DC motor through BTS7960 (R_EN and L_EN are tied to 5V)
// ------------------------------------------------------------------
void motorInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_RPWM, 20000, 8);
  ledcAttach(PIN_LPWM, 20000, 8);
#else
  ledcSetup(0, 20000, 8); ledcAttachPin(PIN_RPWM, 0);
  ledcSetup(1, 20000, 8); ledcAttachPin(PIN_LPWM, 1);
#endif
}
void pwmWrite(int which, int duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(which == 0 ? PIN_RPWM : PIN_LPWM, duty);
#else
  ledcWrite(which, duty);
#endif
}
void motorForward(int duty) { pwmWrite(1, 0); pwmWrite(0, duty); }
void motorStop()            { pwmWrite(0, 0); pwmWrite(1, 0); }

// ------------------------------------------------------------------
// Clock: DS3231 in Philippine time, corrected from internet time
// ------------------------------------------------------------------
bool rtcValidNow(DateTime &now) {
  if (!gRtcOk) return false;
  now = rtc.now();
  return now.year() >= 2025 && now.year() <= 2099;
}

void syncRtcFromInternet() {
  if (!gRtcOk || millis() - tRtcSync < 60000UL) return;   // check once a minute
  tRtcSync = millis();
  uint64_t ms = epochMs();
  if (ms == 0) return;
  uint32_t phNow = (uint32_t)(ms / 1000ULL) + PH_OFFSET_SEC;
  DateTime r = rtc.now();
  bool invalid = rtc.lostPower() || r.year() < 2025 || r.year() > 2099;
  long drift = (long)r.unixtime() - (long)phNow;
  bool daily = !gRtcSyncedOnce || millis() - tRtcDaily > 86400000UL;
  if (invalid || (daily && labs(drift) > 2)) {
    rtc.adjust(DateTime(phNow));
    Serial.printf("[rtc] Clock set from internet time (was off by %ld s).\n", invalid ? 0L : drift);
  }
  if (invalid || daily) { gRtcSyncedOnce = true; tRtcDaily = millis(); }
}

// ------------------------------------------------------------------
// Pigs from the website, feed table and the 5-day adjustment rule
// ------------------------------------------------------------------
uint32_t penSignature(const WebConfig &c) {
  if (!c.hasPen) return 0;
  uint32_t h = 2166136261u;
  int v[5] = {c.penCount, c.penAge, c.penY, c.penM, c.penD};
  for (int i = 0; i < 5; i++) { h ^= (uint32_t)v[i]; h *= 16777619u; }
  return h ? h : 1;
}

// A new pig batch on the website restarts the adjustment
void checkNewBatch(const WebConfig &c) {
  uint32_t sig = penSignature(c);
  if (sig == 0 || sig == gPenSig) return;
  gPenSig = sig;
  prefs.putUInt("penSig", sig);
  S.adjPct = 0; S.histCount = 0; S.lastTarget = 0;
  saveState();
  Serial.printf("[feed] New pigs registered: %d pig(s), %d days old. AI adjustment restarted.\n", c.penCount, c.penAge);
}

// Age today = age when registered + days since the registration date
int currentAge(const WebConfig &c, const DateTime &now) {
  if (!c.hasPen) return 0;
  DateTime reg(c.penY, c.penM, c.penD, 0, 0, 0);
  DateTime today(now.year(), now.month(), now.day(), 0, 0, 0);
  long days = ((long)today.unixtime() - (long)reg.unixtime()) / 86400L;
  if (days < 0) days = 0;
  return c.penAge + (int)days;
}

float adjustedDailyPerPig(int age, int adjPct) {
  int st = stageOf(age);
  float d = dailyPerPig(age) * (1.0f + adjPct / 100.0f);
  float lo = 0.8f * FEED_STAGE_MIN[st], hi = (float)FEED_STAGE_MAX[st];
  if (d < lo) d = lo;
  if (d > hi) d = hi;
  return d;
}
float mealRatio(int idx) { return idx == 0 ? S.amRatio : 1.0f - S.amRatio; }

void pushHist(float leftover, float target) {
  if (S.histCount >= HIST_N) {
    for (int i = 1; i < HIST_N; i++) { S.histLeft[i - 1] = S.histLeft[i]; S.histTgt[i - 1] = S.histTgt[i]; }
    S.histCount = HIST_N - 1;
  }
  S.histLeft[S.histCount] = leftover;
  S.histTgt[S.histCount] = target;
  S.histCount++;
}
void histCounts(int &nEmpty, int &nLeft) {
  nEmpty = 0; nLeft = 0;
  for (int i = 0; i < S.histCount; i++) {
    if (S.histLeft[i] <= EMPTY_FRAC * S.histTgt[i]) nEmpty++;
    if (S.histLeft[i] >  LEFT_FRAC  * S.histTgt[i]) nLeft++;
  }
}

// ------------------------------------------------------------------
// Website link: live readings
// ------------------------------------------------------------------
void publishLive() {
  WebConfig c = cloudGetConfig();
  DateTime now;
  LiveData d = {};
  d.sensorOk = gSensorOk && millis() - gSensorOkAt < 15000UL;   // no reading for 15 s = sensor error
  d.temp = gTemp; d.hum = gHum;
  d.waterOk = gWaterOk;
  d.hopperOk = !isnan(gHopper); d.hopper = gHopper;
  d.trayOk = !isnan(gTray); d.tray = gTray;
  d.rtcValid = rtcValidNow(now);
  if (d.rtcValid) {
    snprintf(d.rtcDate, sizeof(d.rtcDate), "%04d-%02d-%02d", now.year(), now.month(), now.day());
    snprintf(d.rtcTime, sizeof(d.rtcTime), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  }
  d.hasPen = c.hasPen && d.rtcValid;
  if (d.hasPen) { d.ageDays = currentAge(c, now); d.pigs = c.penCount; }
  d.adjPct = S.adjPct;
  strlcpy(d.state, gState, sizeof(d.state));
  cloudPublishLive(d);
}

// ------------------------------------------------------------------
// LCD (20x4 by default)
// ------------------------------------------------------------------
void lcdLine(int row, const char *txt) {
  if (row >= LCD_ROWS) return;
  char buf[21];
  snprintf(buf, sizeof(buf), "%-*.*s", LCD_COLS, LCD_COLS, txt);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

void nextMealInfo(const WebConfig &c, const DateTime &now, int &h, int &m, bool &any) {
  int mins = now.hour() * 60 + now.minute();
  int best = 99999; h = 0; m = 0; any = false;
  for (int i = 0; i < 2; i++) {
    if (!c.slotOn[i]) continue;
    int s = c.slotH[i] * 60 + c.slotMin[i];
    int diff = s - mins; if (diff < 0) diff += 1440;
    if (diff < best) { best = diff; h = c.slotH[i]; m = c.slotMin[i]; any = true; }
  }
}

void updateLcd() {
  WebConfig c = cloudGetConfig();
  DateTime now; bool ok = rtcValidNow(now);
  char b[32];
  bool sensorFresh = gSensorOk && millis() - gSensorOkAt < 15000UL;
  if (!sensorFresh) snprintf(b, sizeof(b), "T:--.-C H:--%% W:%s", gWaterOk ? "OK" : "LOW");
  else snprintf(b, sizeof(b), "T:%4.1fC H:%3.0f%% W:%s", gTemp, gHum, gWaterOk ? "OK" : "LOW");
  lcdLine(0, b);
  if (isnan(gHopper) || isnan(gTray)) snprintf(b, sizeof(b), "Weight sensor error");
  else snprintf(b, sizeof(b), "Hop:%5.1fkg Tr:%4dg", gHopper / 1000.0f, (int)gTray);
  lcdLine(1, b);
  int nh, nm; bool any;
  if (ok) nextMealInfo(c, now, nh, nm, any); else any = false;
  char net = WiFi.status() != WL_CONNECTED ? '-' : (netSignedIn ? 'W' : 'w');
  if (!ok) snprintf(b, sizeof(b), "--:-- RTC error    %c", net);
  else if (!any) snprintf(b, sizeof(b), "%02d:%02d Feeding off  %c", now.hour(), now.minute(), net);
  else snprintf(b, sizeof(b), "%02d:%02d Next %02d:%02d %c", now.hour(), now.minute(), nh, nm, net);
  lcdLine(2, b);
  if (!c.hasPen) snprintf(b, sizeof(b), "No pigs: register");
  else snprintf(b, sizeof(b), "%dp A:%3d %+3d%% %s", c.penCount, ok ? currentAge(c, now) : c.penAge, S.adjPct, gState);
  lcdLine(3, b);
}

// ------------------------------------------------------------------
// Dispensing: motor runs until the HOPPER load cells show the needed drop
// ------------------------------------------------------------------
FeedResult dispenseFeed(float need, const WebConfig &c) {
  FeedResult r = {0, NAN, "OK", 0};
  float start = readHopper(8);
  if (isnan(start)) { r.status = "HOPPER_SENSOR_ERR"; return r; }
  if (start < need + HOPPER_RESERVE_G) {
    r.status = "HOPPER_LOW";
    need = start - HOPPER_RESERVE_G;
    if (need < MIN_DISPENSE_G) return r;
  }
  // Safety stop: the website's motor timeout, longer for big feedings, never over 4 min
  unsigned long limit = (unsigned long)c.motorTimeoutSec * 1000UL;
  unsigned long planned = (unsigned long)(need / c.gramsPerSecond * 1500.0f) + 10000UL;
  if (planned > limit) limit = planned;
  if (limit > FEED_MAX_MS) limit = FEED_MAX_MS;

  float lead = FEED_LEAD_FRAC * need;
  unsigned long t0 = millis(), lastProgress = t0, lastLcd = 0, lastPub = 0;
  float lastMark = start;
  gState = "FEED";
  motorForward(FEED_PWM_FULL);
  while (true) {
    float h = hopperScale.wait_ready_timeout(300) ? hopperScale.get_units(1) : NAN;
    if (!isnan(h)) {
      float done = start - h;
      if (done >= need - lead) break;
      if (lastMark - h > 2.0f) { lastMark = h; lastProgress = millis(); }
      float remaining = need - done;
      motorForward((remaining < 0.15f * need || remaining < 100.0f) ? FEED_PWM_SLOW : FEED_PWM_FULL);
      gHopper = h;
    }
    unsigned long now = millis();
    if (now - t0 > limit) { r.status = "TIMEOUT"; break; }
    if (now - lastProgress > FEED_NOPROGRESS_MS) { r.status = "JAM_OR_EMPTY"; break; }
    if (now - lastLcd > 1000) {
      lastLcd = now;
      char b[24]; snprintf(b, sizeof(b), "Feeding %4d/%4dg", (int)(start - gHopper), (int)need);
      lcdLine(3, b);
    }
    if (now - lastPub > 2000) { lastPub = now; publishLive(); }
  }
  motorStop();
  r.durationMs = millis() - t0;
  delay(FEED_SETTLE_MS);
  float end = readHopper(10);
  r.trayAfter = readTray(10);
  if (!isnan(end)) { r.dispensed = start - end; gHopper = end; }
  if (!isnan(r.trayAfter)) gTray = r.trayAfter;
  if (strcmp(r.status, "OK") == 0 && r.dispensed < 0.9f * need) r.status = "SHORT";
  return r;
}

static bool statusOk(const char *s) { return strcmp(s, "OK") == 0 || strncmp(s, "SKIPPED", 7) == 0; }

static void fillStart(FeedRecord &rec, const DateTime &now) {
  snprintf(rec.rtcStart, sizeof(rec.rtcStart), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  rec.startedAtMs = epochMs();
}

// ------------------------------------------------------------------
// One scheduled meal: read leftover -> adjust -> Random Forest -> dispense -> log
// ------------------------------------------------------------------
void runMeal(int idx, int32_t key, const WebConfig &c, const DateTime &now) {
  S.lastMealKey = key; saveState();
  gState = "MEAL";
  FeedRecord rec = {};
  strlcpy(rec.source, "schedule", sizeof(rec.source));
  strlcpy(rec.slot, idx == 0 ? "am" : "pm", sizeof(rec.slot));
  snprintf(rec.scheduledTime, sizeof(rec.scheduledTime), "%02d:%02d", c.slotH[idx], c.slotMin[idx]);
  rec.scheduled = true;

  // 1) leftover feed = live tray reading right before the meal
  float tray = readTray(15);
  if (isnan(tray)) {
    gState = "TRAYERR";
    fillStart(rec, now);
    rec.ok = false; strlcpy(rec.error, "TRAY_SENSOR_ERR", sizeof(rec.error));
    rec.trayAfter = NAN;
    cloudQueueFeed(rec);
    return;
  }
  if (tray < 0) tray = 0;
  gTray = tray;

  int pigs = c.penCount;
  int age = currentAge(c, now);
  bool ai = c.aiEnabled;

  // 2) record leftover of the previous meal and apply the 5-day rule
  if (S.lastTarget > 0) {
    pushHist(tray, S.lastTarget);
    rec.hasPrevLeftover = true; rec.prevLeftover = tray;
  }
  int nEmpty, nLeft;
  histCounts(nEmpty, nLeft);
  const char *trend = "Maintain";
  if (ai && S.histCount >= HIST_N) {
    if (nEmpty == HIST_N) {
      trend = "Increase";
      int a = S.adjPct + c.aiStepPct; if (a > ADJ_MAX_PCT) a = ADJ_MAX_PCT;
      S.adjPct = a; S.histCount = 0;
    } else if (nLeft == HIST_N) {
      trend = "Decrease";
      int a = S.adjPct - c.aiStepPct; if (a < -ADJ_MAX_PCT) a = -ADJ_MAX_PCT;
      S.adjPct = a; S.histCount = 0;
    }
  }
  int adj = ai ? S.adjPct : 0;

  // 3) meal target for the registered pigs, split into the two meals
  float base = dailyPerPig(age);
  float ratio = mealRatio(idx);
  float target = adjustedDailyPerPig(age, adj) * pigs * ratio;
  float formula = target - tray; if (formula < 0) formula = 0;

  // 4) Random Forest Regression (trained for 3 pigs, 36-140 days).
  //    For 1-2 pigs the inputs are scaled to 3 pigs and the answer scaled back.
  float rfPred = NAN;
  float need = formula;
  const char *decided = "formula";
  if (ai && rfCoversAge(age)) {
    float k = 3.0f / pigs;
    float x[11] = {
      (float)age, (float)stageOf(age), ratio,
      isnan(gTemp) ? 29.0f : gTemp, isnan(gHum) ? 80.0f : gHum,
      base, (float)adj, target * k,
      (float)nEmpty, (float)nLeft, tray * k
    };
    rfPred = rf.predict(x) / k;
    if (rfPred < 0) rfPred = 0;
    if (rfPred > target) rfPred = target;
    if (fabsf(rfPred - formula) <= RF_TRUST_FRAC * target) { need = rfPred; decided = "ai"; }  // else: safety net
  }
  if (tray + need > TRAY_MAX_G) need = TRAY_MAX_G - tray;
  if (need > c.maxGramsPerFeed) need = c.maxGramsPerFeed;   // website "Max per feeding"
  if (need < 0) need = 0;

  Serial.printf("MEAL %s pigs=%d age=%d target=%.0f tray=%.0f rf=%.0f formula=%.0f need=%.0f (%s) trend=%s adj=%d%%\n",
                idx == 0 ? "AM" : "PM", pigs, age, target, tray, isnan(rfPred) ? -1.0f : rfPred, formula, need,
                decided, trend, adj);

  fillStart(rec, now);
  FeedResult res = {0, tray, "SKIPPED_TRAY_FULL", 0};
  if (need >= MIN_DISPENSE_G) res = dispenseFeed(need, c);
  if (isnan(res.trayAfter)) res.trayAfter = tray;

  S.lastTarget = target;
  saveState();
  gState = statusOk(res.status) ? "IDLE" : "ALERT";

  rec.target = strncmp(res.status, "SKIPPED", 7) == 0 ? 0 : need;   // skipped = nothing was asked
  rec.dispensed = res.dispensed;
  rec.durationMs = res.durationMs;
  rec.ok = statusOk(res.status);
  if (strcmp(res.status, "OK") != 0) strlcpy(rec.error, res.status, sizeof(rec.error));
  rec.mealTarget = target; rec.trayBefore = tray; rec.trayAfter = res.trayAfter;
  rec.aiUsed = !isnan(rfPred); rec.aiG = isnan(rfPred) ? 0 : rfPred; rec.formulaG = formula;
  strlcpy(rec.decidedBy, decided, sizeof(rec.decidedBy));
  rec.adjPct = adj; strlcpy(rec.trend, trend, sizeof(rec.trend));
  rec.ageDays = age; rec.pigs = pigs; rec.nEmpty = nEmpty; rec.nLeft = nLeft;
  cloudQueueFeed(rec);
}

// ------------------------------------------------------------------
// "Feed now" from the website (or "feed <grams>" in the Serial Monitor)
// ------------------------------------------------------------------
void runManual(const char *cmdId, float grams, const WebConfig &c) {
  DateTime now; bool ok = rtcValidNow(now);
  unsigned long cool = (unsigned long)c.manualCooldownMin * 60000UL;
  if (cmdId[0] && gManualDone && cool > 0 && millis() - gLastManualAt < cool) {
    char e[48];
    snprintf(e, sizeof(e), "Wait %lu min between manual feedings",
             (unsigned long)((cool - (millis() - gLastManualAt)) / 60000UL + 1));
    cloudCommandStatus(cmdId, "failed", 0, e);
    Serial.printf("[feed] Feed now refused: %s\n", e);
    return;
  }
  if (grams > c.maxGramsPerFeed) grams = c.maxGramsPerFeed;
  cloudCommandStatus(cmdId, "running", 0, nullptr);
  gState = "MANUAL";

  FeedRecord rec = {};
  strlcpy(rec.source, "manual", sizeof(rec.source));
  strlcpy(rec.commandId, cmdId, sizeof(rec.commandId));
  if (ok) fillStart(rec, now); else rec.startedAtMs = epochMs();

  FeedResult res = dispenseFeed(grams, c);
  gLastManualAt = millis(); gManualDone = true;
  bool good = strcmp(res.status, "OK") == 0;
  gState = good ? "IDLE" : "ALERT";
  Serial.printf("[feed] Manual feeding: %.0f of %.0f g (%s)\n", res.dispensed, grams, res.status);

  cloudCommandStatus(cmdId, good ? "done" : "failed", res.dispensed, good ? nullptr : res.status);
  rec.target = grams; rec.dispensed = res.dispensed; rec.durationMs = res.durationMs;
  rec.ok = good;
  if (!good) strlcpy(rec.error, res.status, sizeof(rec.error));
  cloudQueueFeed(rec);
}

// ------------------------------------------------------------------
// Serial commands (calibration and tests), 115200 baud
//   status | tare tray | tare hopper | cal tray <g> | cal hopper <g>
//   feed <g> | motor <seconds> | settime YYYY-MM-DD HH:MM:SS | reset adjust
// ------------------------------------------------------------------
void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n'); line.trim();
  if (!line.length()) return;
  WebConfig c = cloudGetConfig();
  if (line == "status") {
    DateTime n; bool ok = rtcValidNow(n);
    Serial.printf("%s | pigs %s | adj %d%% | T %.1f H %.0f | hopper %.0f g | tray %.0f g | water %s\n",
                  ok ? n.timestamp().c_str() : "RTC error",
                  c.hasPen ? (String(c.penCount) + " x " + String(ok ? currentAge(c, n) : c.penAge) + " days").c_str() : "none",
                  S.adjPct, gTemp, gHum, gHopper, gTray, gWaterOk ? "OK" : "LOW");
    Serial.printf("Wi-Fi %s | Firebase %s | last write HTTP %d | AM %02d:%02d %s | PM %02d:%02d %s | AI %s\n",
                  WiFi.status() == WL_CONNECTED ? "OK" : "no", netSignedIn ? "signed in" : "not signed in", netLastWrite,
                  c.slotH[0], c.slotMin[0], c.slotOn[0] ? "on" : "off",
                  c.slotH[1], c.slotMin[1], c.slotOn[1] ? "on" : "off", c.aiEnabled ? "on" : "off");
  } else if (line == "tare tray") {
    trayScale.set_scale(S.calTray); trayScale.tare(20); S.offTray = trayScale.get_offset(); saveState(); Serial.println("Tray tared");
  } else if (line == "tare hopper") {
    hopperScale.set_scale(S.calHopper); hopperScale.tare(20); S.offHopper = hopperScale.get_offset(); saveState(); Serial.println("Hopper tared");
  } else if (line.startsWith("cal tray ")) {   // put a known weight on the tray first
    float g = line.substring(9).toFloat();
    if (g > 0) { float raw = trayScale.get_value(20); S.calTray = raw / g; trayScale.set_scale(S.calTray); saveState(); Serial.printf("Tray factor %.3f\n", S.calTray); }
  } else if (line.startsWith("cal hopper ")) {  // put a known weight on the hopper first
    float g = line.substring(11).toFloat();
    if (g > 0) { float raw = hopperScale.get_value(20); S.calHopper = raw / g; hopperScale.set_scale(S.calHopper); saveState(); Serial.printf("Hopper factor %.3f\n", S.calHopper); }
  } else if (line.startsWith("feed")) {
    float g = line.length() > 5 ? line.substring(5).toFloat() : 0;
    if (g < 10) { Serial.println("Usage: feed 300   (grams, logged as a manual feeding)"); return; }
    runManual("", g, c);
  } else if (line.startsWith("motor ")) {
    int s = constrain(line.substring(6).toInt(), 1, 20);
    motorForward(FEED_PWM_SLOW); delay(s * 1000UL); motorStop(); Serial.println("Motor test done");
  } else if (line.startsWith("settime ")) {
    int y, mo, d, h, mi, s;
    if (sscanf(line.c_str() + 8, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) { rtc.adjust(DateTime(y, mo, d, h, mi, s)); Serial.println("RTC set"); }
  } else if (line == "reset adjust") {
    S.adjPct = 0; S.histCount = 0; S.lastTarget = 0; saveState(); Serial.println("Adjustment reset");
  } else {
    Serial.println("Commands: status | tare tray | tare hopper | cal tray <g> | cal hopper <g> | feed <g> | motor <s> | settime YYYY-MM-DD HH:MM:SS | reset adjust");
  }
}

// ------------------------------------------------------------------
// Setup / loop
// ------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  lcd.init(); lcd.backlight();
  lcdLine(0, "SMARTPIG starting..");

  pinMode(PIN_WATER, INPUT_PULLUP);
  motorInit(); motorStop();
  RS485.begin(9600, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX);

  gRtcOk = rtc.begin();
  if (!gRtcOk) { lcdLine(1, "RTC NOT FOUND"); Serial.println("DS3231 not found: scheduled feedings are off until it works."); }
  else if (rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));   // internet time corrects it later

  loadState();
  trayScale.begin(PIN_TRAY_DT, PIN_TRAY_SCK);
  hopperScale.begin(PIN_HOPPER_DT, PIN_HOPPER_SCK);
  applyCalibration();

  cloudBegin();   // Wi-Fi + Firebase in the background (core 0)
  lcdLine(1, "Ready. Feed runs on");
  lcdLine(2, "the website schedule");
  delay(1200);
  Serial.println("SMARTPIG ready. Type 'status' or 'help' for commands.");
}

void loop() {
  unsigned long ms = millis();
  handleSerial();
  syncRtcFromInternet();

  // sensors
  if (ms - tSensor > 5000) {
    tSensor = ms;
    float t, h;
    if (readSms2113(t, h)) { gTemp = t; gHum = h; gSensorOk = true; gSensorOkAt = millis(); }
    bool closed = (digitalRead(PIN_WATER) == LOW);
    gWaterOk = (closed == WATER_OK_WHEN_CLOSED);
  }
  if (ms - tWeight > 2000) {
    tWeight = ms;
    float h = readHopper(2), t = readTray(2);
    if (!isnan(h)) { gPrevHopper = h; gHopper = h; } else gHopper = NAN;
    gTray = t;
  }

  WebConfig c = cloudGetConfig();
  checkNewBatch(c);

  // "Feed now" from the website
  ManualCmd cmd;
  if (cloudTakeManual(cmd)) runManual(cmd.id, cmd.grams, c);

  // schedule (DS3231). Needs a working clock and registered pigs.
  DateTime now;
  if (c.hasPen && rtcValidNow(now)) {
    uint32_t dayIdx = now.unixtime() / 86400UL;
    int mins = now.hour() * 60 + now.minute();
    for (int i = 0; i < 2; i++) {
      if (!c.slotOn[i]) continue;
      int sched = c.slotH[i] * 60 + c.slotMin[i];
      int32_t key = (int32_t)dayIdx * 2 + i;
      if (mins >= sched && mins < sched + MEAL_WINDOW_MIN && key > S.lastMealKey) runMeal(i, key, c, now);
    }
  }

  if (millis() - tPublish > 1000) { tPublish = millis(); publishLive(); }
  if (millis() - tLcd > 1000) { tLcd = millis(); updateLcd(); }
}
