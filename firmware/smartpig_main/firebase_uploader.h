#pragma once

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include "secrets.h"
#include "feeder.h"   // NEW: schedule, Feed now, feeding log

#define DEVICE_ID "esp32s3_01"
#define HISTORY_SCHEMA_VERSION 1

struct Snapshot {
  bool ready;
  bool sensorOk;
  float temperature;
  float humidity;
  char waterLevel[16];
  bool waterTop;
  bool waterMiddle;
  bool waterBottom;
  bool rtcValid;
  char rtcDate[11];  // YYYY-MM-DD
  char rtcTime[9];   // HH:MM:SS
};

static Snapshot sharedSnap = {};
static SemaphoreHandle_t snapMutex = nullptr;
static String idToken = "";
static unsigned long tokenObtainedAt = 0;

static const unsigned long SEND_INTERVAL_MS = 5000;           // live snapshot
static const unsigned long HISTORY_INTERVAL_MS = 180000UL;    // history record: 3 minutes
static const unsigned long TOKEN_REFRESH_MS = 50UL * 60UL * 1000UL;
static const unsigned long CONFIG_POLL_MS = 60000UL;          // NEW: schedule + settings
static const unsigned long COMMAND_POLL_MS = 5000UL;          // NEW: "Feed now"

// NEW: test fields (thesis parameters)
static uint32_t historySeq = 0;      // +1 for every history attempt, so gaps = lost records
static bool ntpStarted = false;
static String lastCommandId = "";    // last "Feed now" id we accepted
static String activeCommandId = "";  // id we are reporting status for

// NEW: push ids of recent feeding logs, so the leftover/AI result can update the same record
struct LogLink { uint32_t localId; char pushId[32]; };
static LogLink logLinks[LOG_QUEUE_SIZE] = {};
static uint8_t logLinkNext = 0;

// Called from loop(): copies the latest readings for the network task
void uploaderUpdate(bool sensorOk, float temperature, float humidity,
                    const char *waterLevel, bool top, bool middle, bool bottom,
                    bool rtcValid, const char *rtcDate, const char *rtcTime) {
  if (snapMutex == nullptr) return;
  if (xSemaphoreTake(snapMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;

  sharedSnap.sensorOk = sensorOk;
  sharedSnap.temperature = temperature;
  sharedSnap.humidity = humidity;
  strlcpy(sharedSnap.waterLevel, waterLevel, sizeof(sharedSnap.waterLevel));
  sharedSnap.waterTop = top;
  sharedSnap.waterMiddle = middle;
  sharedSnap.waterBottom = bottom;
  sharedSnap.rtcValid = rtcValid;
  strlcpy(sharedSnap.rtcDate, rtcDate, sizeof(sharedSnap.rtcDate));
  strlcpy(sharedSnap.rtcTime, rtcTime, sizeof(sharedSnap.rtcTime));
  sharedSnap.ready = true;

  xSemaphoreGive(snapMutex);
}

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

static bool uploaderConnectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.println("[net] Connecting to Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[net] Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
    if (!ntpStarted) {   // NEW: real time for sentAt / startedAt (UTC)
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      ntpStarted = true;
    }
    return true;
  }

  Serial.println("[net] Wi-Fi failed. Will retry.");
  return false;
}

static bool uploaderSignIn() {
  WiFiClientSecure client;
  client.setInsecure();  // TEST ONLY: certificate not verified

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(8000);

  String url =
      String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") +
      FIREBASE_API_KEY;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  JsonDocument req;
  req["email"] = DEVICE_EMAIL;
  req["password"] = DEVICE_PASSWORD;
  req["returnSecureToken"] = true;

  String body;
  serializeJson(req, body);

  int code = http.POST(body);
  String response = http.getString();
  http.end();

  if (code != 200) {
    Serial.print("[net] Sign-in failed. HTTP ");
    Serial.println(code);
    return false;
  }

  JsonDocument res;
  if (deserializeJson(res, response)) {
    Serial.println("[net] Sign-in failed: bad response.");
    return false;
  }

  idToken = res["idToken"].as<String>();
  tokenObtainedAt = millis();
  Serial.println("[net] Firebase sign-in OK.");
  return true;
}

// NEW: one helper for every Realtime Database call (GET, PUT, POST, PATCH)
static int fbRequest(const char *method, const String &path, const String &body, String *response) {
  WiFiClientSecure client;
  client.setInsecure();  // TEST ONLY

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(8000);

  String url = String("https://") + FIREBASE_DB_HOST + path + "?auth=" + idToken;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  int code = http.sendRequest(method, body);
  String res = http.getString();
  http.end();

  if (code == 401 || code == 403) idToken = "";  // force new sign-in
  if (code != 200) {
    Serial.printf("[net] %s %s -> HTTP %d\n", method, path.c_str(), code);
    Serial.println(res);
  }
  if (response) *response = res;
  return code;
}

// Builds the JSON body. history=false -> live snapshot, history=true -> history record
static String buildBody(const Snapshot &s, bool history) {
  char tempText[12];
  char humText[12];

  JsonDocument doc;
  doc["sensorOk"] = s.sensorOk;

  // Failed sensor = the fields are left out. Never send an old or zero value.
  if (s.sensorOk) {
    snprintf(tempText, sizeof(tempText), "%.1f", s.temperature);
    snprintf(humText, sizeof(humText), "%.1f", s.humidity);
    doc["temperature"] = serialized(tempText);
    doc["humidity"] = serialized(humText);
  }

  doc["waterLevel"] = s.waterLevel;
  doc["waterTop"] = s.waterTop;
  doc["waterMiddle"] = s.waterMiddle;
  doc["waterBottom"] = s.waterBottom;

  doc["rtcValid"] = s.rtcValid;
  if (s.rtcValid) {
    doc["rtcDate"] = s.rtcDate;
    doc["rtcTime"] = s.rtcTime;
  }

  // NEW: stability fields (System page -> Test results)
  doc["bootCount"] = feederBootCount;
  doc["uptimeSec"] = (uint32_t)(millis() / 1000);
  doc["freeHeap"] = (uint32_t)ESP.getFreeHeap();
  doc["resetReason"] = resetReasonText();

  // NEW: load cells (left out until the scale is calibrated and answering)
  float g;
  if (hopperGrams(g)) doc["hopperGrams"] = roundf(g);
  if (trayGrams(g)) doc["trayGrams"] = roundf(g);

  if (history) {
    doc["deviceId"] = DEVICE_ID;
    doc["schemaVersion"] = HISTORY_SCHEMA_VERSION;
    doc["seq"] = historySeq;                      // NEW: transmission test
    uint64_t t = epochMs();
    if (t > 0) doc["sentAt"] = t;                 // NEW: delay test (only with NTP)
    doc["recordedAt"][".sv"] = "timestamp";       // Firebase server time
  } else {
    doc["lastUpdated"][".sv"] = "timestamp";      // Firebase server time
  }

  String body;
  serializeJson(doc, body);
  return body;
}

// PUT replaces /smartpig. POST adds a new record under /smartpigHistory.
static int uploaderSend(const Snapshot &s, bool history) {
  String body = buildBody(s, history);
  int code = fbRequest(history ? "POST" : "PUT",
                       history ? "/smartpigHistory.json" : "/smartpig.json", body, nullptr);
  Serial.print(history ? "[net] History write -> HTTP " : "[net] Write -> HTTP ");
  Serial.println(code);
  return code;
}

// ---------------- NEW: feeding ----------------

// Downloads the schedule and settings. Applies them when the admin changed them,
// then tells the dashboard "synced" by copying updatedAt into deviceAck.
static void pollConfig() {
  String res;
  if (fbRequest("GET", "/smartpigConfig.json", "", &res) != 200) return;
  if (res == "null") return;

  JsonDocument doc;
  if (deserializeJson(doc, res)) return;

  uint64_t updatedAt = doc["updatedAt"] | (uint64_t)0;
  uint64_t deviceAck = doc["deviceAck"] | (uint64_t)0;
  if (updatedAt == 0) return;   // nothing saved from the dashboard yet

  if (updatedAt != feederConfigVersion()) feederApplyConfig(res);

  if (deviceAck != updatedAt) {
    char ack[24];
    snprintf(ack, sizeof(ack), "%llu", (unsigned long long)updatedAt);
    if (fbRequest("PUT", "/smartpigConfig/deviceAck.json", ack, nullptr) == 200) {
      Serial.println("[net] Schedule synced (deviceAck).");
    }
  }
}

// Checks for a new "Feed now" request from the dashboard
static void pollCommand() {
  String res;
  if (fbRequest("GET", "/smartpigCommands/feedNow.json", "", &res) != 200) return;
  if (res == "null") return;

  JsonDocument doc;
  if (deserializeJson(doc, res)) return;

  const char *status = doc["status"] | "";
  const char *id = doc["id"] | "";
  float grams = doc["grams"] | 0.0f;
  uint64_t requestedAt = doc["requestedAt"] | (uint64_t)0;

  if (strcmp(status, "pending") != 0 || id[0] == '\0') return;
  if (lastCommandId == id) return;   // already handled

  lastCommandId = id;
  feedPrefs.putString("lastCmd", lastCommandId);   // survives a reboot

  uint64_t now = epochMs();
  if (now > 0 && requestedAt > 0 && now > requestedAt + COMMAND_TTL_MS) {
    Serial.println("[net] Feed now request too old, ignored.");
    return;   // the dashboard shows it as expired
  }

  Serial.printf("[net] Feed now: %.0f g (id %s)\n", grams, id);
  activeCommandId = id;
  feederSetManualCommand(id, grams);
}

// Reports running / done / failed back to the dashboard
static void sendCommandResult() {
  CommandResult r;
  if (!feederTakeCommandResult(r)) return;

  // Don't overwrite a newer request someone made in the meantime
  String current;
  if (fbRequest("GET", "/smartpigCommands/feedNow/id.json", "", &current) != 200) {
    feederRequeueCommandResult(r);
    return;
  }
  if (current != String("\"") + r.id + "\"") return;

  JsonDocument doc;
  doc["status"] = r.status;
  if (strcmp(r.status, "running") == 0) {
    doc["startedAt"][".sv"] = "timestamp";
  } else {
    doc["dispensedGrams"] = roundf(r.dispensedGrams * 10) / 10;
    doc["finishedAt"][".sv"] = "timestamp";
    if (r.error[0]) doc["error"] = r.error;
  }

  String body;
  serializeJson(doc, body);
  if (fbRequest("PATCH", "/smartpigCommands/feedNow.json", body, nullptr) != 200) {
    feederRequeueCommandResult(r);
  }
}

// Uploads finished feedings to /smartpigFeedLog (Tables 4.2–4.4)
static void sendFeedLogs() {
  FeedRecord r;
  while (feederPeekLog(r)) {
    JsonDocument doc;
    doc["source"] = r.source;
    if (r.slot[0]) doc["slot"] = r.slot;
    if (r.commandId[0]) doc["commandId"] = r.commandId;
    if (r.scheduledTime[0]) doc["scheduledTime"] = r.scheduledTime;
    if (r.rtcStart[0]) doc["rtcStart"] = r.rtcStart;
    if (r.startedAtMs > 0) doc["startedAt"] = r.startedAtMs;
    else doc["startedAt"][".sv"] = "timestamp";
    doc["finishedAt"][".sv"] = "timestamp";
    doc["targetGrams"] = roundf(r.targetGrams);
    doc["dispensedGrams"] = roundf(r.dispensedGrams * 10) / 10;
    doc["durationMs"] = r.durationMs;
    doc["method"] = r.loadcell ? "loadcell" : "timed";
    doc["ok"] = r.ok;
    if (r.error[0]) doc["error"] = r.error;

    String body, res;
    serializeJson(doc, body);
    if (fbRequest("POST", "/smartpigFeedLog.json", body, &res) != 200) return;  // retry later
    feederPopLog();

    // Firebase answers {"name":"-Nx..."}: remember it for the leftover update
    JsonDocument rd;
    if (!deserializeJson(rd, res)) {
      LogLink &l = logLinks[logLinkNext];
      l.localId = r.localId;
      strlcpy(l.pushId, rd["name"] | "", sizeof(l.pushId));
      logLinkNext = (logLinkNext + 1) % LOG_QUEUE_SIZE;
    }
    Serial.println("[net] Feeding log uploaded.");
  }
}

// NEW: adds leftoverGrams (and the AI nextGrams) to the feeding record
static void sendObservations() {
  ObsResult r;
  while (feederPeekObs(r)) {
    const char *pushId = nullptr;
    for (auto &l : logLinks) {
      if (l.localId == r.localId && l.pushId[0]) pushId = l.pushId;
    }
    if (!pushId) {
      // Record not uploaded yet: wait, unless its log is no longer queued (then give up)
      FeedRecord pending;
      if (feederPeekLog(pending)) return;
      feederPopObs();
      continue;
    }

    JsonDocument doc;
    doc["leftoverGrams"] = roundf(r.leftoverGrams * 10) / 10;
    if (r.nextGrams > 0) doc["nextGrams"] = r.nextGrams;
    String body;
    serializeJson(doc, body);
    if (fbRequest("PATCH", String("/smartpigFeedLog/") + pushId + ".json", body, nullptr) != 200) return;
    feederPopObs();
    Serial.println("[net] Leftover uploaded.");
  }
}

static void uploaderTask(void *param) {
  unsigned long lastSend = 0;
  unsigned long lastHistory = millis();  // first history record after one full interval
  unsigned long lastConfig = 0;
  unsigned long lastCommand = 0;
  bool configChecked = false;

  for (;;) {
    if (!uploaderConnectWiFi()) {
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    if (idToken == "" || millis() - tokenObtainedAt > TOKEN_REFRESH_MS) {
      if (!uploaderSignIn()) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }
    }

    // NEW: feeding status first, so the dashboard updates quickly
    sendCommandResult();
    sendFeedLogs();
    sendObservations();

    if (millis() - lastCommand >= COMMAND_POLL_MS) {
      lastCommand = millis();
      pollCommand();
    }

    if (!configChecked || millis() - lastConfig >= CONFIG_POLL_MS) {
      lastConfig = millis();
      configChecked = true;
      pollConfig();
    }

    if (millis() - lastSend >= SEND_INTERVAL_MS) {
      lastSend = millis();

      Snapshot copy;
      bool haveData = false;

      if (xSemaphoreTake(snapMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        copy = sharedSnap;
        haveData = copy.ready;
        xSemaphoreGive(snapMutex);
      }

      if (haveData) {
        uploaderSend(copy, false);

        // CHANGED: history is sent every interval even if the live write failed,
        // and seq counts every attempt, so missing seq numbers = lost records.
        if (millis() - lastHistory >= HISTORY_INTERVAL_MS) {
          lastHistory = millis();
          historySeq++;
          uploaderSend(copy, true);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void uploaderBegin() {
  snapMutex = xSemaphoreCreateMutex();
  lastCommandId = feedPrefs.getString("lastCmd", "");   // feederBegin() must run first
  xTaskCreatePinnedToCore(uploaderTask, "uploader", 16384, nullptr, 1, nullptr, 0);
}
