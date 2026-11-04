/*
  SMARTPIG - IoT-Based Automated Feeding and Real-Time Monitoring System
  with Rule-Based AI Precision Feeding (3 fattening pigs, Buhi, Camarines Sur)

  Board   : ESP32-S3-WROOM-2-N32R16V (Arduino core for ESP32)
  Libraries (Library Manager):
    - HX711 by Bogdan Necula (bogde)
    - RTClib by Adafruit
    - LiquidCrystal I2C by Frank de Brabander
    - ArduinoJson by Benoit Blanchon (version 7.x)
  Built in: WiFi, WiFiClientSecure, HTTPClient, Preferences, Wire

  Files in this folder (keep them together):
    SMARTPIG_ESP32.ino, config.h, feed_table.h, RandomForestRegressor.h, cloud.h,
    rf_test_vectors.h (optional, enables the 'rftest' serial command)

  WEBSITE VERSION: instead of a local server, the ESP32 talks to the SmartPig website
  through Firebase (cloud.h, runs in the background on core 0):
    - live readings twice a second (real time), a history record every 3 min
    - pigs (age + count), AM/PM times, AI on/off and Max per feeding come from the website
    - the website's "Feed now" dispenses the asked grams
    - every feeding is saved to the website's Feeding log
  Wi-Fi and the Firebase device login are in config.h.
*/

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#include <LiquidCrystal_I2C.h>
#include <HX711.h>

#include "config.h"

// The hopper factor comes from DEFAULT_HOPPER_CAL_PER_G in config.h. When you change it there and
// upload, the new value replaces the one saved on the ESP32 (see loadState). "cal hopper <grams>" also works.

#include "feed_table.h"
#include "RandomForestRegressor.h"
#define RF_N_FEATURES 12                    // must match FEATURES in train_export_rf.py
#if __has_include("rf_test_vectors.h")
#include "rf_test_vectors.h"
static_assert(RF_TEST_FEATURES == RF_N_FEATURES, "rf_test_vectors.h and the sketch disagree on the feature count");
#endif
#include "cloud.h"                          // Firebase link to the website

#define PH_OFFSET_SEC (8L * 3600L)          // Philippine time = UTC + 8, no daylight saving
// ------------------------------------------------------------------
// Objects
// ------------------------------------------------------------------
HardwareSerial RS485(1);
HX711 trayScale, hopperScale;
RTC_DS3231 rtc;
LiquidCrystal_I2C lcd(LCD_ADDR, 20, 4);
Preferences prefs;
Eloquent::ML::Port::RandomForestRegressor rf;

// ------------------------------------------------------------------
// Saved state (survives power loss)
// ------------------------------------------------------------------
#define STATE_MAGIC 0x5350494Au  // "SPIJ"
struct State {
  uint32_t magic;
  float calTray, calHopper;      // grams per HX711 count (set by calibration)
  long offTray, offHopper;       // HX711 zero offsets (set by tare)
  int16_t adjPct;                // current feed adjustment, -20..+20 %
  uint8_t histCount;             // number of valid leftover records
  float histLeft[HIST_N];        // leftover (g) read before the next meal
  float histTgt[HIST_N];         // the meal target (g) that leftover belongs to
  float lastTarget;              // target of the most recent meal
  int32_t lastMealKey;           // dayIndex*2 + mealIndex of the last meal done
  int16_t baseAge;               // pig age on baseDay
  uint32_t baseDay;              // day index (unixtime/86400) when age == baseAge
  uint8_t mealH[2], mealM[2];    // meal times
  float amRatio;                 // morning share of the daily feed
  bool enabled;                  // feeding enabled from the web app
} S;

// ------------------------------------------------------------------
// Live values
// ------------------------------------------------------------------
float gTemp = NAN, gHum = NAN;       // last good SMS2113 reading
float gHopper = NAN, gTray = NAN;    // grams
float gPrevHopper = NAN;
bool gWaterOk = true;
bool gRefillFlag = false;
const char *gState = "IDLE";
float gLastDispensed = 0, gLastRequested = 0;
int gNextTarget = 0;

unsigned long tSensor = 0, tWeight = 0, tLcd = 0, tReport = 0, tConfig = 0, tWifi = 0;
bool gFirstConfigTried = false;
int gPigs = DEFAULT_PIGS;            // pig count, set from the web app (kept in its own flash key)

// Website link
bool gSensorOk = false;              // last SMS2113 read worked
unsigned long gSensorOkAt = 0;
bool gRtcOk = false;
uint32_t gPenSig = 0;                // which pig batch the saved age belongs to
unsigned long gLastManualAt = 0;
int16_t gRanHM[2] = {-1, -1};          // AM / PM time (minutes) that already ran on the day of S.lastMealKey
bool gManualDone = false;
unsigned long tPublish = 0, tRtcSync = 0, tRtcDaily = 0;
bool gRtcSyncedOnce = false;

// ------------------------------------------------------------------
// Helpers: state storage
// ------------------------------------------------------------------
void setDefaults() {
  memset(&S, 0, sizeof(S));
  S.magic = STATE_MAGIC;
  S.calTray = DEFAULT_TRAY_CAL_PER_G;
  S.calHopper = DEFAULT_HOPPER_CAL_PER_G;
  S.adjPct = 0;
  S.lastMealKey = -1;
  S.baseAge = DEFAULT_START_AGE;
  S.baseDay = 0;  // set after the RTC is read
  S.mealH[0] = DEFAULT_MEAL0_H; S.mealM[0] = DEFAULT_MEAL0_M;
  S.mealH[1] = DEFAULT_MEAL1_H; S.mealM[1] = DEFAULT_MEAL1_M;
  S.amRatio = DEFAULT_AM_RATIO;
  S.enabled = true;
}

void saveState() { prefs.putBytes("state", &S, sizeof(S)); }

// The pig count lives in its own flash key, so adding it does not change the saved State struct
// (a changed struct size would wipe the calibration).
void loadPigs() {
  int p = prefs.getUChar("pigs", DEFAULT_PIGS);
  gPigs = (p >= PIGS_MIN && p <= PIGS_MAX) ? p : DEFAULT_PIGS;
}
// Returns true if the value was accepted. A new herd size makes the old leftover history misleading,
// so that history is cleared (the learned adjustment % is kept: it is per pig).
bool setPigs(int p) {
  if (p < PIGS_MIN || p > PIGS_MAX) return false;
  if (p == gPigs) return true;
  gPigs = p;
  prefs.putUChar("pigs", (uint8_t)p);
  S.histCount = 0; S.lastTarget = 0;
  saveState();
  return true;
}

void loadState() {
  prefs.begin("smartpig", false);
  size_t n = prefs.getBytesLength("state");
  bool loaded = false;
  if (n == sizeof(S)) {
    prefs.getBytes("state", &S, sizeof(S));
    loaded = (S.magic == STATE_MAGIC);
  }
  if (!loaded) setDefaults();
  // A new DEFAULT_HOPPER_CAL_PER_G in config.h replaces the factor saved in flash once.
  // A later "cal hopper <grams>" is kept until the code's default changes again.
  float usedDefault = prefs.getFloat("hcalDef", 0.0f);
  if (usedDefault != DEFAULT_HOPPER_CAL_PER_G) {
    S.calHopper = DEFAULT_HOPPER_CAL_PER_G;
    prefs.putFloat("hcalDef", DEFAULT_HOPPER_CAL_PER_G);
    saveState();
    Serial.printf("Hopper factor set from config.h: %.4f counts per gram\n", S.calHopper);
  }
}

// ------------------------------------------------------------------
// Weight sensors (HX711)
// ------------------------------------------------------------------
// Weights are never negative: drift or noise below zero is shown and reported as 0 (NaN = sensor error).
float noNegative(float v) { return (!isnan(v) && v <= 0.0f) ? 0.0f : v; }

float safeWeight(HX711 &sc, int samples) {
  if (!sc.wait_ready_timeout(1000)) return NAN;   // sensor not answering
  return noNegative(sc.get_units(samples));
}
float readTray(int n = 5)   { return safeWeight(trayScale, n); }
float readHopper(int n = 5) { return safeWeight(hopperScale, n); }

// Steadies the live hopper / tray numbers (LCD + website). Load cells always wobble by
// a few grams; this keeps the last 8 readings (4 s), drops the highest and lowest,
// and averages the rest. A real change (refill, feed taken out) of more than
// WEIGHT_JUMP_G is shown at once instead of being slowed down.
// Feeding itself does NOT use this: the motor loop reads the hopper directly.
#define WEIGHT_AVG_N   8
#define WEIGHT_JUMP_G  300.0f
#define WEIGHT_STEP_G  5.0f        // shown value is rounded to 5 g so the last digit stops flickering
struct WeightFilter {
  float buf[WEIGHT_AVG_N]; int n = 0, pos = 0; float out = NAN;
  float add(float v) {
    if (isnan(v)) { n = 0; pos = 0; out = NAN; return NAN; }
    if (!isnan(out) && fabsf(v - out) > WEIGHT_JUMP_G) { n = 0; pos = 0; }   // real change: start over
    buf[pos] = v; pos = (pos + 1) % WEIGHT_AVG_N; if (n < WEIGHT_AVG_N) n++;
    float sum = 0, lo = buf[0], hi = buf[0];
    for (int i = 0; i < n; i++) { sum += buf[i]; lo = fminf(lo, buf[i]); hi = fmaxf(hi, buf[i]); }
    float avg = (n >= 4) ? (sum - lo - hi) / (n - 2) : sum / n;
    float r = roundf(avg / WEIGHT_STEP_G) * WEIGHT_STEP_G;
    if (isnan(out) || fabsf(r - out) >= WEIGHT_STEP_G) out = r;   // ignore changes smaller than 5 g
    return noNegative(out);
  }
};
WeightFilter hopperFilter, trayFilter;

void applyCalibration() {
  trayScale.set_scale(S.calTray);   trayScale.set_offset(S.offTray);
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
// Pig age, feed table and the 5-day adjustment rule
// ------------------------------------------------------------------
int currentAge(const DateTime &now) {
  long a = (long)S.baseAge + (long)(now.unixtime() / 86400UL) - (long)S.baseDay;
  if (a < FEED_AGE_MIN) a = FEED_AGE_MIN;
  if (a > FEED_AGE_MAX) a = FEED_AGE_MAX;
  return (int)a;
}
int stageOf(int age) { return age <= 50 ? 1 : age <= 80 ? 2 : age <= 115 ? 3 : 4; }

float dailyPerPig(int age) {  // base value from the B-MEG chart
  return (float)FEED_PER_PIG[age - FEED_AGE_MIN];
}
float adjustedDailyPerPig(int age) {
  int st = stageOf(age);
  float d = dailyPerPig(age) * (1.0f + S.adjPct / 100.0f);
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
// Web communication
// ------------------------------------------------------------------
bool wifiUp() { return WiFi.status() == WL_CONNECTED; }

// The DS3231 keeps Philippine time; internet time (NTP, started by cloud.h) corrects it
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

bool rtcValidNow(DateTime &now) {
  if (!gRtcOk) return false;
  now = rtc.now();
  return now.year() >= 2025 && now.year() <= 2099;
}

// Website settings -> this sketch's saved state (same effect as the old /api/config)
//   Register pigs (count + age + date) -> gPigs, S.baseAge, S.baseDay (new batch restarts the adjustment)
//   AM / PM times                      -> S.mealH / S.mealM
//   No pigs registered (Finish)        -> scheduled feeding off (S.enabled = false)
static uint32_t penSignature(const WebConfig &c) {
  if (!c.hasPen) return 0;
  uint32_t h = 2166136261u;
  int v[5] = {c.penCount, c.penAge, c.penY, c.penM, c.penD};
  for (int i = 0; i < 5; i++) { h ^= (uint32_t)v[i]; h *= 16777619u; }
  return h ? h : 1;
}

void applyWebConfig(const WebConfig &c) {
  bool changed = false;
  if (c.hasPen && c.penCount != gPigs && setPigs(c.penCount))
    Serial.printf("[web] Pig count set to %d\n", gPigs);

  uint32_t sig = penSignature(c);
  if (sig != 0 && sig != gPenSig) {          // new pigs registered on the website
    gPenSig = sig;
    prefs.putUInt("penSig", sig);
    S.baseAge = c.penAge;
    S.baseDay = DateTime(c.penY, c.penM, c.penD, 0, 0, 0).unixtime() / 86400UL;
    S.adjPct = 0; S.histCount = 0; S.lastTarget = 0;
    changed = true;
    Serial.printf("[web] New pigs: %d pig(s), %d days old on %04d-%02d-%02d. Adjustment restarted.\n",
                  c.penCount, c.penAge, c.penY, c.penM, c.penD);
  }
  for (int i = 0; i < 2; i++) {
    if (S.mealH[i] != c.slotH[i] || S.mealM[i] != c.slotMin[i]) {
      S.mealH[i] = c.slotH[i]; S.mealM[i] = c.slotMin[i]; changed = true;
    }
  }
  if (S.amRatio != DEFAULT_AM_RATIO) { S.amRatio = DEFAULT_AM_RATIO; changed = true; }   // website: 50 / 50
  if (S.enabled != c.hasPen) { S.enabled = c.hasPen; changed = true; }
  if (changed) saveState();
}

// Live readings for the website (sent twice a second by cloud.h)
void publishLive() {
  WebConfig c = cloudGetConfig();
  DateTime now;
  LiveData d = {};
  d.sensorOk = gSensorOk && millis() - gSensorOkAt < 15000UL;   // no good reading for 15 s = sensor error
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
  if (d.hasPen) { d.ageDays = currentAge(now); d.pigs = gPigs; }
  d.adjPct = S.adjPct;
  strlcpy(d.state, gState, sizeof(d.state));
  cloudPublishLive(d);
}

static void fillStart(FeedRecord &rec) {
  DateTime now;
  if (rtcValidNow(now))
    snprintf(rec.rtcStart, sizeof(rec.rtcStart), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  rec.startedAtMs = epochMs();
}
static bool statusOk(const char *st) { return strcmp(st, "OK") == 0 || strncmp(st, "SKIPPED", 7) == 0; }

// ------------------------------------------------------------------
// LCD 20x4
// ------------------------------------------------------------------
void lcdLine(int row, const char *txt) {
  char buf[21];
  snprintf(buf, sizeof(buf), "%-20.20s", txt);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

void nextMealInfo(const DateTime &now, int &h, int &m) {
  int mins = now.hour() * 60 + now.minute();
  int best = 99999; h = S.mealH[0]; m = S.mealM[0];
  for (int i = 0; i < 2; i++) {
    int s = S.mealH[i] * 60 + S.mealM[i];
    int diff = s - mins; if (diff < 0) diff += 1440;
    if (diff < best) { best = diff; h = S.mealH[i]; m = S.mealM[i]; }
  }
}

void updateLcd() {
  DateTime now = rtc.now();
  char b[32];
  if (isnan(gTemp)) snprintf(b, sizeof(b), "T:--.-C H:--%% W:%s", gWaterOk ? "OK" : "LOW");
  else snprintf(b, sizeof(b), "T:%4.1fC H:%3.0f%% W:%s", gTemp, gHum, gWaterOk ? "OK" : "LOW");
  lcdLine(0, b);
  if (isnan(gHopper) || isnan(gTray)) snprintf(b, sizeof(b), "Weight sensor error");
  else snprintf(b, sizeof(b), "H:%5.2fkg T:%4.2fkg", gHopper / 1000.0f, gTray / 1000.0f);
  lcdLine(1, b);
  int nh, nm; nextMealInfo(now, nh, nm);
  snprintf(b, sizeof(b), "%02d:%02d Next %02d:%02d %c", now.hour(), now.minute(), nh, nm, wifiUp() ? 'W' : '-');
  lcdLine(2, b);
  snprintf(b, sizeof(b), "A:%3d P%d %+d%% %s", currentAge(now), gPigs, S.adjPct, gState);
  lcdLine(3, b);
}

// ------------------------------------------------------------------
// Dispensing: motor runs until the HOPPER load cells show the needed drop
// ------------------------------------------------------------------

FeedResult dispenseFeed(float need) {
  FeedResult r = {0, NAN, "OK"};
  float start = readHopper(8);
  if (isnan(start)) { r.status = "HOPPER_SENSOR_ERR"; return r; }
  if (start < need + HOPPER_RESERVE_G) {
    r.status = "HOPPER_LOW";
    need = start - HOPPER_RESERVE_G;
    if (need < MIN_DISPENSE_G) { return r; }
  }
  float lead = FEED_LEAD_FRAC * need;
  unsigned long t0 = millis(), lastProgress = t0, lastLcd = 0;
  float lastMark = start;
  gState = "FEED";
  motorForward(FEED_PWM_FULL);
  while (true) {
    float h = hopperScale.wait_ready_timeout(300) ? noNegative(hopperScale.get_units(1)) : NAN;
    if (!isnan(h)) {
      float done = start - h;
      if (done >= need - lead) break;
      if (lastMark - h > 2.0f) { lastMark = h; lastProgress = millis(); }
      float remaining = need - done;
      motorForward((remaining < 0.15f * need || remaining < 100.0f) ? FEED_PWM_SLOW : FEED_PWM_FULL);
      gHopper = h;
    }
    unsigned long now = millis();
    if (now - t0 > FEED_MAX_MS) { r.status = "TIMEOUT"; break; }
    if (now - lastProgress > FEED_NOPROGRESS_MS) { r.status = "JAM_OR_EMPTY"; break; }
    if (now - lastLcd > 1000) {
      lastLcd = now;
      char b[24]; snprintf(b, sizeof(b), "Feeding %4d/%4dg", (int)(start - gHopper), (int)need);
      lcdLine(3, b);
      publishLive();                       // the website sees "FEED" and the hopper going down
    }
  }
  motorStop();
  delay(FEED_SETTLE_MS);
  float end = readHopper(10);
  r.trayAfter = readTray(10);
  if (!isnan(end)) r.dispensed = start - end;
  if (strcmp(r.status, "OK") == 0 && r.dispensed < 0.9f * need) r.status = "SHORT";
  return r;
}

// ------------------------------------------------------------------
// One complete meal: read leftover -> adjust -> Random Forest -> dispense -> log
// ------------------------------------------------------------------
void runMeal(int idx, int32_t key, bool manual) {
  DateTime now = rtc.now();
  WebConfig c = cloudGetConfig();
  if (!manual) {
    S.lastMealKey = key; saveState();
    gRanHM[idx] = S.mealH[idx] * 60 + S.mealM[idx];          // this slot's time that has now run today
    prefs.putShort(idx == 0 ? "ranHM0" : "ranHM1", gRanHM[idx]);
  }
  gState = "MEAL";

  FeedRecord rec = {};
  strlcpy(rec.source, manual ? "manual" : "schedule", sizeof(rec.source));
  if (!manual) {
    strlcpy(rec.slot, idx == 0 ? "am" : "pm", sizeof(rec.slot));
    snprintf(rec.scheduledTime, sizeof(rec.scheduledTime), "%02d:%02d", S.mealH[idx], S.mealM[idx]);
  }
  rec.scheduled = true;

  // 1) leftover feed = live tray reading right before the meal.
  // Tray load cell not ready / not connected, or reading 0 -> counted as "no leftover", feeding goes on.
  float tray = readTray(15);
  if (isnan(tray)) {
    Serial.println("[feed] Tray load cell not ready: counted as no leftover (0 g).");
    tray = 0;
  }
  if (tray < 0) tray = 0;
  gTray = tray;

  int age = currentAge(now);

  bool ai = c.aiEnabled;                    // website: Advanced settings -> AI feeding

  // 2) record leftover of the previous meal and apply the 5-day rule
  if (!manual && S.lastTarget > 0) {
    pushHist(tray, S.lastTarget);
    rec.hasPrevLeftover = true; rec.prevLeftover = tray;   // shown as that meal's Leftover on the website
  }
  int nEmpty, nLeft;
  histCounts(nEmpty, nLeft);
  const char *trend = "Maintain";
  if (ai && !manual && S.histCount >= HIST_N) {
    if (nEmpty == HIST_N) {
      trend = "Increase";
      int a = S.adjPct + ADJ_STEP_PCT; if (a > ADJ_MAX_PCT) a = ADJ_MAX_PCT;
      S.adjPct = a; S.histCount = 0;
    } else if (nLeft == HIST_N) {
      trend = "Decrease";
      int a = S.adjPct - ADJ_STEP_PCT; if (a < -ADJ_MAX_PCT) a = -ADJ_MAX_PCT;
      S.adjPct = a; S.histCount = 0;
    }
  }

  // 3) meal target for gPigs pigs, split into the two meals
  float base = dailyPerPig(age);
  float ratio = mealRatio(idx);
  float target = adjustedDailyPerPig(age) * gPigs * ratio;
  float formula = target - tray; if (formula < 0) formula = 0;

  // 4) Random Forest Regression (same order as FEATURES in train_export_rf.py).
  // 12 slots: Pig_count is the last one. A forest exported with 11 inputs simply never reads x[11].
  float x[RF_N_FEATURES] = {
    (float)age,                   // x[0]  Age_days
    (float)stageOf(age),          // x[1]  Stage_code
    ratio,                        // x[2]  Meal_ratio
    isnan(gTemp) ? 29.0f : gTemp, // x[3]  Temperature_C
    isnan(gHum) ? 80.0f : gHum,   // x[4]  Humidity_percent
    base,                         // x[5]  Daily_feed_per_pig_g
    (float)S.adjPct,              // x[6]  Feed_adjust_percent
    target,                       // x[7]  Meal_target_g
    (float)nEmpty,                // x[8]  Empty_meals_last_10
    (float)nLeft,                 // x[9]  Leftover_meals_last_10
    tray,                         // x[10] Tray_Load_G
    (float)gPigs                  // x[11] Pig_count
  };

  float rfPred = rf.predict(x);
  if (rfPred < 0) rfPred = 0;
  if (rfPred > target) rfPred = target;
  float need = formula;
  const char *decided = "formula";
  if (ai && fabsf(rfPred - formula) <= RF_TRUST_FRAC * target) { need = rfPred; decided = "ai"; }  // else safety net
  if (tray + need > TRAY_MAX_G) need = TRAY_MAX_G - tray;
  if (need > c.maxGramsPerFeed) need = c.maxGramsPerFeed;   // website: Max per feeding
  if (need < 0) need = 0;

  Serial.printf("MEAL %s pigs=%d age=%d target=%.0f tray=%.0f rf=%.0f formula=%.0f need=%.0f trend=%s adj=%d%%\n",
                idx == 0 ? "AM" : "PM", gPigs, age, target, tray, rfPred, formula, need, trend, S.adjPct);

  fillStart(rec);
  unsigned long tStart = millis();
  FeedResult res = {0, tray, "SKIPPED_TRAY_FULL"};
  if (need >= MIN_DISPENSE_G) res = dispenseFeed(need);
  if (isnan(res.trayAfter)) res.trayAfter = tray;
  rec.durationMs = millis() - tStart;

  if (!manual) { S.lastTarget = target; }
  saveState();
  gLastRequested = need;
  gLastDispensed = res.dispensed;
  gState = (strcmp(res.status, "OK") == 0 || strncmp(res.status, "SKIPPED", 7) == 0) ? "IDLE" : "ALERT";

  // Feeding log on the website
  rec.target = strncmp(res.status, "SKIPPED", 7) == 0 ? 0 : need;
  rec.dispensed = res.dispensed;
  rec.ok = statusOk(res.status);
  if (strcmp(res.status, "OK") != 0) strlcpy(rec.error, res.status, sizeof(rec.error));
  rec.mealTarget = target; rec.trayBefore = tray; rec.trayAfter = res.trayAfter;
  rec.aiUsed = true; rec.aiG = rfPred; rec.formulaG = formula;
  strlcpy(rec.decidedBy, decided, sizeof(rec.decidedBy));
  rec.adjPct = S.adjPct; strlcpy(rec.trend, trend, sizeof(rec.trend));
  rec.ageDays = age; rec.pigs = gPigs; rec.nEmpty = nEmpty; rec.nLeft = nLeft;
  cloudQueueFeed(rec);
}

// ------------------------------------------------------------------
// "Feed now" from the website: dispense exactly the asked grams
// ------------------------------------------------------------------
void runFeedNow(const char *cmdId, float grams) {
  WebConfig c = cloudGetConfig();          // no waiting time between manual feedings
  if (grams > c.maxGramsPerFeed) grams = c.maxGramsPerFeed;
  cloudCommandStatus(cmdId, "running", 0, nullptr);
  gState = "MANUAL";

  FeedRecord rec = {};
  strlcpy(rec.source, "manual", sizeof(rec.source));
  strlcpy(rec.commandId, cmdId, sizeof(rec.commandId));
  fillStart(rec);
  unsigned long tStart = millis();
  FeedResult res = dispenseFeed(grams);
  rec.durationMs = millis() - tStart;
  gLastManualAt = millis(); gManualDone = true;
  bool good = strcmp(res.status, "OK") == 0;
  gState = good ? "IDLE" : "ALERT";
  Serial.printf("[web] Feed now: %.0f of %.0f g (%s)\n", res.dispensed, grams, res.status);

  cloudCommandStatus(cmdId, good ? "done" : "failed", res.dispensed, good ? nullptr : res.status);
  rec.target = grams; rec.dispensed = res.dispensed; rec.ok = good;
  if (!good) strlcpy(rec.error, res.status, sizeof(rec.error));
  cloudQueueFeed(rec);
}

// ------------------------------------------------------------------
// Forest self-test: runs the test rows saved by train_export_rf.py through the exported forest
// on the ESP32 and compares with Python's answers. Raw model output, no clipping or safety net.
// ------------------------------------------------------------------
void rfSelfTest() {
#ifdef RF_TEST_N
  const float TOL_G = 0.01f;
  int bad = 0; float worst = 0;
  for (int i = 0; i < RF_TEST_N; i++) {
    float x[RF_N_FEATURES];
    for (int k = 0; k < RF_N_FEATURES; k++) x[k] = RF_TEST_X[i][k];
    float got = rf.predict(x);
    float diff = fabsf(got - RF_TEST_Y[i]);
    if (diff > worst) worst = diff;
    bool ok = diff <= TOL_G;
    if (!ok) bad++;
    Serial.printf("rf %2d pigs=%d python=%9.3f esp32=%9.3f diff=%.4f %s\n",
                  i, (int)x[RF_N_FEATURES - 1], RF_TEST_Y[i], got, diff, ok ? "ok" : "MISMATCH");
  }
  Serial.printf("RF SELFTEST %s: %d/%d rows within %.2f g, worst diff %.4f g\n",
                bad == 0 ? "PASS" : "FAIL", RF_TEST_N - bad, RF_TEST_N, TOL_G, worst);
#else
  Serial.println("rf_test_vectors.h not found: run train_export_rf.py and copy it next to the sketch");
#endif
}

// ------------------------------------------------------------------
// Serial commands (calibration and tests), 115200 baud
//   status | tare tray | tare hopper | cal tray <g> | cal hopper <g>
//   feed | motor <seconds> | settime YYYY-MM-DD HH:MM:SS | reset adjust | pigs <n> | rftest
// ------------------------------------------------------------------
void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n'); line.trim();
  if (!line.length()) return;
  if (line == "status") {
    DateTime n = rtc.now();
    Serial.printf("%s | age %d | pigs %d | adj %d%% | T %.1f H %.0f | hopper %.0f g | tray %.0f g | water %s | wifi %s\n",
                  n.timestamp().c_str(), currentAge(n), gPigs, S.adjPct, gTemp, gHum, gHopper, gTray,
                  gWaterOk ? "OK" : "LOW", wifiUp() ? "yes" : "no");
    WebConfig c = cloudGetConfig();
    Serial.printf("Firebase %s | last write HTTP %d | pigs on website: %s | AM %02d:%02d %s | PM %02d:%02d %s | AI %s\n",
                  netSignedIn ? "signed in" : "not signed in", netLastWrite, c.hasPen ? "yes" : "no",
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
  } else if (line == "feed") {
    DateTime n = rtc.now(); runMeal(n.hour() < 12 ? 0 : 1, -1, true);
  } else if (line.startsWith("motor ")) {
    int s = constrain(line.substring(6).toInt(), 1, 20);
    motorForward(FEED_PWM_SLOW); delay(s * 1000UL); motorStop(); Serial.println("Motor test done");
  } else if (line.startsWith("settime ")) {
    int y, mo, d, h, mi, s;
    if (sscanf(line.c_str() + 8, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) { rtc.adjust(DateTime(y, mo, d, h, mi, s)); Serial.println("RTC set"); }
  } else if (line == "rftest") {
    rfSelfTest();
  } else if (line.startsWith("pigs ")) {   // bench test; the web app value replaces it at the next config fetch
    int p = line.substring(5).toInt();
    if (setPigs(p)) Serial.printf("Pig count = %d\n", gPigs); else Serial.printf("Pig count must be %d..%d\n", PIGS_MIN, PIGS_MAX);
  } else if (line == "reset adjust") {
    S.adjPct = 0; S.histCount = 0; S.lastTarget = 0; saveState(); Serial.println("Adjustment reset");
  } else {
    Serial.println("Commands: status | tare tray | tare hopper | cal tray <g> | cal hopper <g> | feed | motor <s> | settime YYYY-MM-DD HH:MM:SS | reset adjust | pigs <n> | rftest");
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
  if (!gRtcOk) { lcdLine(1, "RTC NOT FOUND"); Serial.println("DS3231 not found"); }
  else if (rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));   // internet time corrects it later

  loadState();
  loadPigs();
  gRanHM[0] = prefs.getShort("ranHM0", -1);
  gRanHM[1] = prefs.getShort("ranHM1", -1);
  gPenSig = prefs.getUInt("penSig", 0);
  if (S.baseDay == 0) { S.baseDay = rtc.now().unixtime() / 86400UL; saveState(); }

  trayScale.begin(PIN_TRAY_DT, PIN_TRAY_SCK);
  hopperScale.begin(PIN_HOPPER_DT, PIN_HOPPER_SCK);
  applyCalibration();

  cloudBegin();                // Wi-Fi + Firebase in the background (core 0)
  applyWebConfig(cloudGetConfig());   // last saved website settings, so feeding works offline
  lcdLine(1, "Ready. Feed runs on");
  lcdLine(2, "the website schedule");
  delay(1200);
  Serial.println("SMARTPIG ready. Type 'help' for commands.");
}

void loop() {
  unsigned long ms = millis();
  handleSerial();

  // sensors
  if (ms - tSensor >= 500) {                  // temperature, humidity, water twice a second
    tSensor = ms;
    float t, h;
    if (readSms2113(t, h)) { gTemp = t; gHum = h; gSensorOk = true; gSensorOkAt = millis(); }
    bool closed = (digitalRead(PIN_WATER) == LOW);
    gWaterOk = (closed == WATER_OK_WHEN_CLOSED);
  }
  if (ms - tWeight >= 500) {                  // hopper + tray twice a second (LCD and website)
    tWeight = ms;
    float h = hopperFilter.add(readHopper(1)), t = trayFilter.add(readTray(1));
    if (!isnan(h)) {
      if (!isnan(gPrevHopper) && h - gPrevHopper > REFILL_JUMP_G) gRefillFlag = true;
      gPrevHopper = h; gHopper = h;
    } else gHopper = NAN;
    gTray = t;
  }

  // Website: settings, clock, "Feed now"
  syncRtcFromInternet();
  WebConfig cfg = cloudGetConfig();
  applyWebConfig(cfg);
  ManualCmd cmd;
  if (cloudTakeManual(cmd)) runFeedNow(cmd.id, cmd.grams);

  // schedule (DS3231). Needs registered pigs on the website and a working clock.
  DateTime now;
  if (rtcValidNow(now)) {
    uint32_t dayIdx = now.unixtime() / 86400UL;
    int mins = now.hour() * 60 + now.minute();
    // If the clock was once set to a later date, the saved "last meal" would block every meal until
    // that date comes. Anything more than a day ahead of today is treated as wrong and cleared.
    if (S.lastMealKey > (int32_t)dayIdx * 2 + 1) {
      Serial.println("[feed] Saved last-meal date is in the future (clock was changed): cleared.");
      S.lastMealKey = -1; saveState();
    }
    static int32_t skipNoted = -1, doneNoted = -1;
    for (int i = 0; i < 2; i++) {
      if (!cfg.slotOn[i]) continue;                       // website: AM / PM switch
      int sched = S.mealH[i] * 60 + S.mealM[i];
      int32_t key = (int32_t)dayIdx * 2 + i;
      bool inWindow = mins >= sched && mins < sched + MEAL_WINDOW_MIN;
      // Runs once per slot per day. If the time was changed on the website after it ran today,
      // the new time runs too (gRanHM remembers which time already ran).
      bool notYet = key > S.lastMealKey || (key == S.lastMealKey && gRanHM[i] != sched);
      bool due = inWindow && notYet;
      if (inWindow && !notYet && doneNoted != key) {
        doneNoted = key;
        Serial.printf("[feed] %s %02d:%02d already fed today.\n", i == 0 ? "AM" : "PM", S.mealH[i], S.mealM[i]);
      }
      if (due && S.enabled) runMeal(i, key, false);
      else if (due && skipNoted != key) {                 // say once why a meal time was skipped
        skipNoted = key;
        Serial.printf("[feed] %s %02d:%02d skipped: no pigs registered on the website.\n",
                      i == 0 ? "AM" : "PM", S.mealH[i], S.mealM[i]);
      }
    }
  }

  if (millis() - tPublish >= 250) { tPublish = millis(); publishLive(); }

  if (ms - tLcd >= 500) { tLcd = ms; updateLcd(); }
}
