#pragma once
// SMARTPIG <-> website link (Firebase Realtime Database over HTTPS).
// Runs in its own task on core 0, so a slow network never stops the motor
// or the schedule (those run in loop() on core 1).
//
// Reads : /smartpigConfig          pigs (age + count), AM/PM times, settings
//         /smartpigCommands/feedNow "Feed now" from the website
// Writes: /smartpig                 live readings twice a second (real time)
//         /smartpigHistory          one record every 3 min (charts, tests)
//         /smartpigFeedLog          one record per feeding (+ leftover later)
//         /smartpigConfig/deviceAck "the feeder has the latest schedule"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_system.h>
#include <sys/time.h>
// Wi-Fi and Firebase login come from config.h (WIFI_SSID, WIFI_PASS, FIREBASE_API_KEY,
// FIREBASE_DB_HOST, DEVICE_EMAIL, DEVICE_PASSWORD). The device account must be the one the
// database rules trust (UID vP6dp...), never a person's website account.
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD WIFI_PASS
#endif
#ifndef MAX_PIGS
#ifdef PIGS_MAX
#define MAX_PIGS PIGS_MAX
#else
#define MAX_PIGS 3
#endif
#endif

#define HISTORY_SCHEMA_VERSION 2
#define COMMAND_TTL_MS (2UL * 60UL * 1000UL)   // same as the website

static const unsigned long SEND_INTERVAL_MS = 500UL;    // live reading to the website twice a second (real time)
static const unsigned long HISTORY_INTERVAL_MS = 180000UL;
static const unsigned long TOKEN_REFRESH_MS = 50UL * 60UL * 1000UL;
static const unsigned long CONFIG_POLL_MS = 60000UL;
static const unsigned long COMMAND_POLL_MS = 5000UL;

// ------------------------------------------------------------------
// Settings from the website
// ------------------------------------------------------------------
struct WebConfig {
  uint64_t updatedAt;
  bool hasPen;
  int penCount, penAge, penY, penM, penD;
  bool slotOk[2];               // 0 = am, 1 = pm
  bool slotOn[2];
  int slotH[2], slotMin[2];
  float maxGramsPerFeed;
  float gramsPerSecond;
  int motorTimeoutSec;
  int manualCooldownMin;
  bool aiEnabled;
  int aiStepPct;
};

static void webConfigDefaults(WebConfig &c) {
  memset(&c, 0, sizeof(c));
  c.slotOk[0] = c.slotOk[1] = true;
  c.slotOn[0] = c.slotOn[1] = true;
  c.slotH[0] = 7;  c.slotMin[0] = 0;    // website default 07:00
  c.slotH[1] = 17; c.slotMin[1] = 0;    // website default 17:00
  c.maxGramsPerFeed = 4000;
  c.gramsPerSecond = 20;
  c.motorTimeoutSec = 90;
  c.manualCooldownMin = 10;
  c.aiEnabled = true;
  c.aiStepPct = ADJ_STEP_PCT;
}

static bool parseWebConfig(const String &json, WebConfig &out) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  webConfigDefaults(out);

  JsonObject st = doc["settings"];
  if (!st.isNull()) {
    out.maxGramsPerFeed = st["maxGramsPerFeed"] | out.maxGramsPerFeed;
    out.gramsPerSecond = st["gramsPerSecond"] | out.gramsPerSecond;
    out.motorTimeoutSec = st["motorTimeoutSec"] | out.motorTimeoutSec;
    out.manualCooldownMin = st["manualCooldownMin"] | out.manualCooldownMin;
    out.aiEnabled = st["aiEnabled"] | out.aiEnabled;
    out.aiStepPct = st["aiStepPct"] | out.aiStepPct;
  }
  if (out.gramsPerSecond <= 0) out.gramsPerSecond = 20;
  if (out.motorTimeoutSec <= 0) out.motorTimeoutSec = 90;
  if (out.maxGramsPerFeed <= 0) out.maxGramsPerFeed = 4000;
  if (out.aiStepPct < 1 || out.aiStepPct > ADJ_MAX_PCT) out.aiStepPct = ADJ_STEP_PCT;

  JsonObject pen = doc["pen"];
  if (!pen.isNull()) {
    int count = pen["count"] | 0;
    int age = pen["ageDays"] | 0;
    const char *date = pen["registeredDate"] | "";
    int y = 0, mo = 0, d = 0;
    if (count >= 1 && count <= MAX_PIGS && age >= 1 && sscanf(date, "%d-%d-%d", &y, &mo, &d) == 3) {
      out.hasPen = true;
      out.penCount = count; out.penAge = age;
      out.penY = y; out.penM = mo; out.penD = d;
    }
  }

  JsonObject sch = doc["schedule"];
  const char *ids[2] = {"am", "pm"};
  for (int i = 0; i < 2; i++) {
    JsonObject s = sch[ids[i]];
    if (s.isNull()) continue;
    const char *t = s["time"] | "";
    int h = -1, m = -1;
    if (sscanf(t, "%d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
      out.slotH[i] = h; out.slotMin[i] = m;
    }
    out.slotOn[i] = s["enabled"] | true;
  }
  out.updatedAt = doc["updatedAt"] | (uint64_t)0;
  return true;
}

// ------------------------------------------------------------------
// Data shared between loop() and the network task
// ------------------------------------------------------------------
struct LiveData {
  bool ready;
  bool sensorOk; float temp, hum;
  bool waterOk;
  bool hopperOk; float hopper;
  bool trayOk; float tray;
  bool rtcValid; char rtcDate[11]; char rtcTime[9];
  bool hasPen; int ageDays; int pigs;
  int adjPct;
  char state[12];
};

struct ManualCmd { bool pending; char id[41]; float grams; };
struct CmdResult { char id[41]; char status[8]; float grams; char error[48]; };

struct FeedRecord {
  char source[9];          // "schedule" | "manual"
  char slot[3];            // "am" | "pm"
  char commandId[41];
  char scheduledTime[6];   // "HH:MM"
  char rtcStart[9];        // "HH:MM:SS"
  uint64_t startedAtMs;    // 0 = use server time
  uint32_t durationMs;
  float target, dispensed;
  bool ok; char error[24];
  bool scheduled;          // the AI fields below are filled
  float mealTarget, trayBefore, trayAfter, aiG, formulaG;
  bool aiUsed; char decidedBy[8];
  int adjPct; char trend[10];
  int ageDays, pigs, nEmpty, nLeft;
  bool hasPrevLeftover; float prevLeftover;   // tray before this meal = leftover of the last one
};

#define FEED_QUEUE 6
#define RESULT_QUEUE 4

static SemaphoreHandle_t cloudMutex = nullptr;
static WebConfig webCfg;
static LiveData liveShared = {};
static ManualCmd manualShared = {};
static FeedRecord feedQueue[FEED_QUEUE]; static uint8_t feedCount = 0;
static CmdResult resultQueue[RESULT_QUEUE]; static uint8_t resultCount = 0;
static Preferences cloudPrefs;
static uint32_t bootCount = 0;

// Network status for the "status" command and the LCD
static volatile bool netSignedIn = false;
static volatile int netLastWrite = 0;

static bool lockCloud(uint32_t ms = 50) { return cloudMutex && xSemaphoreTake(cloudMutex, pdMS_TO_TICKS(ms)) == pdTRUE; }
static void unlockCloud() { xSemaphoreGive(cloudMutex); }

// Real time from NTP (UTC ms). 0 until synced.
uint64_t epochMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1700000000) return 0;
  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}

// ---- called from loop() ----
WebConfig cloudGetConfig() {
  WebConfig c;
  if (lockCloud(100)) { c = webCfg; unlockCloud(); }
  else c = webCfg;   // rare; worst case a half-updated copy for one loop
  return c;
}
void cloudPublishLive(const LiveData &d) {
  if (!lockCloud(20)) return;
  liveShared = d; liveShared.ready = true;
  unlockCloud();
}
bool cloudTakeManual(ManualCmd &out) {
  bool got = false;
  if (lockCloud(20)) {
    if (manualShared.pending) { out = manualShared; manualShared.pending = false; got = true; }
    unlockCloud();
  }
  return got;
}
void cloudCommandStatus(const char *id, const char *status, float grams, const char *error) {
  if (!id || !id[0]) return;
  if (!lockCloud(200)) return;
  if (resultCount == RESULT_QUEUE) {   // drop the oldest
    memmove(&resultQueue[0], &resultQueue[1], sizeof(CmdResult) * (RESULT_QUEUE - 1));
    resultCount--;
  }
  CmdResult &r = resultQueue[resultCount++];
  memset(&r, 0, sizeof(r));
  strlcpy(r.id, id, sizeof(r.id));
  strlcpy(r.status, status, sizeof(r.status));
  r.grams = grams;
  if (error) strlcpy(r.error, error, sizeof(r.error));
  unlockCloud();
}
void cloudQueueFeed(const FeedRecord &rec) {
  if (!lockCloud(200)) return;
  if (feedCount == FEED_QUEUE) {
    memmove(&feedQueue[0], &feedQueue[1], sizeof(FeedRecord) * (FEED_QUEUE - 1));
    feedCount--;
  }
  feedQueue[feedCount++] = rec;
  unlockCloud();
}

// ------------------------------------------------------------------
// Firebase helpers
// ------------------------------------------------------------------
static String idToken = "";
static unsigned long tokenAt = 0;
static bool ntpStarted = false;
static uint32_t historySeq = 0;
static String lastCommandId = "";

static const char *resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_EXT:       return "EXTERNAL";
    default:                return "OTHER";
  }
}

static bool netWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  netSignedIn = false;
  Serial.println("[net] Connecting to Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) vTaskDelay(pdMS_TO_TICKS(500));
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[net] Wi-Fi failed. Check WIFI_SSID / WIFI_PASS in config.h (2.4 GHz only). Will retry.");
    return false;
  }
  Serial.print("[net] Wi-Fi connected. IP: ");
  Serial.println(WiFi.localIP());
  if (!ntpStarted) { configTime(0, 0, "pool.ntp.org", "time.google.com"); ntpStarted = true; }
  return true;
}

static bool netSignIn() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setConnectTimeout(5000); http.setTimeout(8000);
  http.begin(client, String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") + FIREBASE_API_KEY);
  http.addHeader("Content-Type", "application/json");
  JsonDocument req;
  req["email"] = DEVICE_EMAIL; req["password"] = DEVICE_PASSWORD; req["returnSecureToken"] = true;
  String body; serializeJson(req, body);
  int code = http.POST(body);
  String res = http.getString();
  http.end();
  if (code != 200) {
    Serial.printf("[net] Sign-in failed. HTTP %d (check DEVICE_EMAIL / DEVICE_PASSWORD / API key)\n", code);
    Serial.println(res);   // Firebase's reason, e.g. INVALID_LOGIN_CREDENTIALS
    return false;
  }
  JsonDocument d;
  if (deserializeJson(d, res)) { Serial.println("[net] Sign-in failed: bad response."); return false; }
  idToken = d["idToken"].as<String>();
  tokenAt = millis();
  netSignedIn = true;
  Serial.printf("[net] Firebase sign-in OK. Device UID: %s\n", (d["localId"] | "?"));
  return true;
}

// One secure connection to the database is kept open and reused (keep-alive).
// Opening a new HTTPS connection takes about 1 s on the ESP32; reusing it takes
// about 0.1-0.3 s, which is what makes the live readings close to real time.
static WiFiClientSecure dbClient;
static HTTPClient dbHttp;
static bool dbReady = false;

static int fb(const char *method, const String &path, const String &body, String *response) {
  if (!dbReady) {
    dbClient.setInsecure();
    dbHttp.setReuse(true);
    dbHttp.setConnectTimeout(5000);
    dbHttp.setTimeout(8000);
    dbReady = true;
  }
  dbHttp.begin(dbClient, String("https://") + FIREBASE_DB_HOST + path + "?auth=" + idToken);
  dbHttp.addHeader("Content-Type", "application/json");
  int code = dbHttp.sendRequest(method, body);
  String res = code > 0 ? dbHttp.getString() : String();
  dbHttp.end();                       // keeps the connection open for the next request
  if (code <= 0) dbClient.stop();     // broken connection: open a fresh one next time
  if (code == 401) { idToken = ""; netSignedIn = false; }
  if (code != 200) {
    Serial.printf("[net] %s %s -> HTTP %d %s\n", method, path.c_str(), code, res.c_str());
    if (code == 401 || code == 403)
      Serial.println("[net] Permission denied: publish the latest database rules, and use the device account the rules trust.");
  }
  if (response) *response = res;
  return code;
}

static void putR1(JsonDocument &d, const char *k, float v) { d[k] = roundf(v * 10) / 10; }

// ---- live + history ----
static String liveBody(const LiveData &s, bool history) {
  JsonDocument d;
  char tb[12], hb[12];   // must live until serializeJson below
  d["sensorOk"] = s.sensorOk;
  if (s.sensorOk) {   // a failed sensor sends nothing, never an old or zero value
    snprintf(tb, sizeof(tb), "%.1f", s.temp);
    snprintf(hb, sizeof(hb), "%.1f", s.hum);
    d["temperature"] = serialized(tb);
    d["humidity"] = serialized(hb);
  }
  // One float switch: the website's tank shows FULL (water OK) or BELOW LOW
  d["waterLevel"] = s.waterOk ? "FULL" : "BELOW LOW";
  d["waterTop"] = s.waterOk; d["waterMiddle"] = s.waterOk; d["waterBottom"] = s.waterOk;
  d["rtcValid"] = s.rtcValid;
  if (s.rtcValid) { d["rtcDate"] = s.rtcDate; d["rtcTime"] = s.rtcTime; }
  d["bootCount"] = bootCount;
  d["uptimeSec"] = (uint32_t)(millis() / 1000);
  d["freeHeap"] = (uint32_t)ESP.getFreeHeap();
  d["resetReason"] = resetReasonText();
  if (s.hopperOk) d["hopperGrams"] = roundf(s.hopper);
  if (s.trayOk) d["trayGrams"] = roundf(s.tray);
  if (s.hasPen) { d["ageDays"] = s.ageDays; d["pigs"] = s.pigs; }
  d["adjustPct"] = s.adjPct;
  d["state"] = s.state;
  if (history) {
    d["deviceId"] = DEVICE_ID;
    d["schemaVersion"] = HISTORY_SCHEMA_VERSION;
    d["seq"] = historySeq;
    uint64_t t = epochMs();
    if (t > 0) d["sentAt"] = t;
    d["recordedAt"][".sv"] = "timestamp";
  } else {
    d["lastUpdated"][".sv"] = "timestamp";
  }
  String body; serializeJson(d, body);
  return body;
}

// ---- schedule + settings ----
static void pollConfig() {
  String res;
  if (fb("GET", "/smartpigConfig.json", "", &res) != 200 || res == "null") return;
  JsonDocument doc;
  if (deserializeJson(doc, res)) return;
  uint64_t updatedAt = doc["updatedAt"] | (uint64_t)0;
  uint64_t ack = doc["deviceAck"] | (uint64_t)0;
  if (updatedAt == 0) return;

  WebConfig next;
  if (updatedAt != webCfg.updatedAt && parseWebConfig(res, next)) {
    if (lockCloud(200)) { webCfg = next; unlockCloud(); }
    cloudPrefs.putString("cfg", res);   // feeding keeps working offline
    Serial.printf("[net] Settings updated: %s, AM %02d:%02d %s, PM %02d:%02d %s, AI %s\n",
                  next.hasPen ? "pigs registered" : "NO pigs registered",
                  next.slotH[0], next.slotMin[0], next.slotOn[0] ? "on" : "off",
                  next.slotH[1], next.slotMin[1], next.slotOn[1] ? "on" : "off",
                  next.aiEnabled ? "on" : "off");
  }
  if (ack != updatedAt) {
    char b[24]; snprintf(b, sizeof(b), "%llu", (unsigned long long)updatedAt);
    if (fb("PUT", "/smartpigConfig/deviceAck.json", b, nullptr) == 200) Serial.println("[net] Schedule synced (deviceAck).");
  }
}

// ---- "Feed now" ----
static void pollCommand() {
  String res;
  if (fb("GET", "/smartpigCommands/feedNow.json", "", &res) != 200 || res == "null") return;
  JsonDocument d;
  if (deserializeJson(d, res)) return;
  const char *status = d["status"] | "";
  const char *id = d["id"] | "";
  float grams = d["grams"] | 0.0f;
  uint64_t requestedAt = d["requestedAt"] | (uint64_t)0;
  if (strcmp(status, "pending") != 0 || !id[0] || lastCommandId == id) return;
  lastCommandId = id;
  cloudPrefs.putString("lastCmd", lastCommandId);
  uint64_t now = epochMs();
  if (now > 0 && requestedAt > 0 && now > requestedAt + COMMAND_TTL_MS) {
    Serial.println("[net] Feed now request too old, ignored.");
    return;
  }
  Serial.printf("[net] Feed now: %.0f g\n", grams);
  if (lockCloud(200)) {
    manualShared.pending = true;
    strlcpy(manualShared.id, id, sizeof(manualShared.id));
    manualShared.grams = grams;
    unlockCloud();
  }
}

static void sendResults() {
  while (true) {
    CmdResult r;
    if (!lockCloud()) return;
    bool have = resultCount > 0;
    if (have) r = resultQueue[0];
    unlockCloud();
    if (!have) return;

    String cur;   // don't overwrite a newer request
    if (fb("GET", "/smartpigCommands/feedNow/id.json", "", &cur) != 200) return;
    if (cur == String("\"") + r.id + "\"") {
      JsonDocument d;
      d["status"] = r.status;
      if (strcmp(r.status, "running") == 0) d["startedAt"][".sv"] = "timestamp";
      else {
        putR1(d, "dispensedGrams", r.grams);
        d["finishedAt"][".sv"] = "timestamp";
        if (r.error[0]) d["error"] = r.error;
      }
      String body; serializeJson(d, body);
      if (fb("PATCH", "/smartpigCommands/feedNow.json", body, nullptr) != 200) return;
    }
    if (lockCloud(200)) {
      memmove(&resultQueue[0], &resultQueue[1], sizeof(CmdResult) * (RESULT_QUEUE - 1));
      resultCount--;
      unlockCloud();
    }
  }
}

// ---- feeding log ----
static void sendFeeds() {
  while (true) {
    FeedRecord r;
    if (!lockCloud()) return;
    bool have = feedCount > 0;
    if (have) r = feedQueue[0];
    unlockCloud();
    if (!have) return;

    // 1) leftover of the previous scheduled meal (the tray right before this meal)
    if (r.hasPrevLeftover) {
      String prev = cloudPrefs.getString("lastSched", "");
      if (prev.length()) {
        JsonDocument p; putR1(p, "leftoverGrams", r.prevLeftover < 0 ? 0 : r.prevLeftover);
        String body; serializeJson(p, body);
        int c = fb("PATCH", String("/smartpigFeedLog/") + prev + ".json", body, nullptr);
        if (c != 200 && c != 400 && c != 403) return;   // retry later (400/403: give up on it)
      }
      r.hasPrevLeftover = false;
      if (lockCloud(200)) { if (feedCount > 0) feedQueue[0].hasPrevLeftover = false; unlockCloud(); }
    }

    // 2) this meal
    JsonDocument d;
    d["source"] = r.source;
    if (r.slot[0]) d["slot"] = r.slot;
    if (r.commandId[0]) d["commandId"] = r.commandId;
    if (r.scheduledTime[0]) d["scheduledTime"] = r.scheduledTime;
    if (r.rtcStart[0]) d["rtcStart"] = r.rtcStart;
    if (r.startedAtMs > 0) d["startedAt"] = r.startedAtMs; else d["startedAt"][".sv"] = "timestamp";
    d["finishedAt"][".sv"] = "timestamp";
    d["targetGrams"] = roundf(r.target);
    putR1(d, "dispensedGrams", r.dispensed);
    d["durationMs"] = r.durationMs;
    d["method"] = "loadcell";
    d["ok"] = r.ok;
    if (r.error[0]) d["error"] = r.error;
    if (r.scheduled) {
      d["mealTargetGrams"] = roundf(r.mealTarget);
      putR1(d, "trayBeforeGrams", r.trayBefore);
      if (!isnan(r.trayAfter)) putR1(d, "trayAfterGrams", r.trayAfter);
      if (r.aiUsed) d["aiGrams"] = roundf(r.aiG);
      d["formulaGrams"] = roundf(r.formulaG);
      d["decidedBy"] = r.decidedBy;
      d["adjustPct"] = r.adjPct;
      d["trend"] = r.trend;
      d["ageDays"] = r.ageDays;
      d["pigs"] = r.pigs;
      d["emptyMeals"] = r.nEmpty;
      d["leftoverMeals"] = r.nLeft;
    }
    String body, res; serializeJson(d, body);
    int c = fb("POST", "/smartpigFeedLog.json", body, &res);
    if (c != 200 && c != 400) return;   // offline: retry later. 400 = bad record, drop it
    if (c == 200 && r.scheduled) {
      JsonDocument rd;
      if (!deserializeJson(rd, res)) cloudPrefs.putString("lastSched", rd["name"] | "");
    }
    Serial.println(c == 200 ? "[net] Feeding log uploaded." : "[net] Feeding log refused (400), dropped.");
    if (lockCloud(200)) {
      memmove(&feedQueue[0], &feedQueue[1], sizeof(FeedRecord) * (FEED_QUEUE - 1));
      feedCount--;
      unlockCloud();
    }
  }
}

// ------------------------------------------------------------------
// The network task
// ------------------------------------------------------------------
static void cloudTask(void *) {
  unsigned long lastSend = 0, lastHistory = millis(), lastConfig = 0, lastCommand = 0;
  bool configChecked = false;
  for (;;) {
    if (!netWifi()) { vTaskDelay(pdMS_TO_TICKS(5000)); continue; }
    if (idToken == "" || millis() - tokenAt > TOKEN_REFRESH_MS) {
      if (!netSignIn()) { vTaskDelay(pdMS_TO_TICKS(10000)); continue; }
    }

    sendResults();
    sendFeeds();

    if (millis() - lastCommand >= COMMAND_POLL_MS) { lastCommand = millis(); pollCommand(); }
    if (!configChecked || millis() - lastConfig >= CONFIG_POLL_MS) {
      lastConfig = millis(); configChecked = true; pollConfig();
    }
    if (millis() - lastSend >= SEND_INTERVAL_MS) {
      lastSend = millis();
      LiveData copy; bool have = false;
      if (lockCloud()) { copy = liveShared; have = copy.ready; unlockCloud(); }
      if (have) {
        netLastWrite = fb("PUT", "/smartpig.json", liveBody(copy, false), nullptr);
        if (millis() - lastHistory >= HISTORY_INTERVAL_MS) {
          lastHistory = millis();
          historySeq++;
          fb("POST", "/smartpigHistory.json", liveBody(copy, true), nullptr);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// Call once in setup(), before reading cloudGetConfig()
void cloudBegin() {
  cloudMutex = xSemaphoreCreateMutex();
  cloudPrefs.begin("cloud", false);
  bootCount = cloudPrefs.getUInt("boots", 0) + 1;
  cloudPrefs.putUInt("boots", bootCount);
  lastCommandId = cloudPrefs.getString("lastCmd", "");
  webConfigDefaults(webCfg);
  String saved = cloudPrefs.getString("cfg", "");
  if (saved.length()) {
    WebConfig c;
    if (parseWebConfig(saved, c)) webCfg = c;
    Serial.printf("[net] Using the last saved settings (%s).\n", webCfg.hasPen ? "pigs registered" : "no pigs registered");
  }
  xTaskCreatePinnedToCore(cloudTask, "cloud", 16384, nullptr, 1, nullptr, 0);
}
