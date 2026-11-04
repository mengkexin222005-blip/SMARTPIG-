#pragma once

// SmartPig load cells (two HX711 modules). Library: "HX711 Arduino Library" by Bogdan Necula.
//
// From the schematic:
//   H1 HX711 -> junction box J1 -> 4 cantilever load cells (L1–L4)   DAT = GPIO 12, CLK = GPIO 13
//   H2 HX711 -> 1 cantilever load cell (L6)                          DAT = GPIO 10, CLK = GPIO 11
//   Both HX711 VCC = 3V3, GND = GND.
//
// ASSUMED: the 4-cell platform carries the HOPPER and the single cell carries the TRAY.
// If it's the other way round, swap the two pin pairs below.

#include <Arduino.h>
#include <HX711.h>
#include <Preferences.h>

#define HOPPER_DOUT_PIN 12   // H1 DAT
#define HOPPER_SCK_PIN  13   // H1 CLK
#define TRAY_DOUT_PIN   10   // H2 DAT
#define TRAY_SCK_PIN    11   // H2 CLK

#define SCALE_AVG 5                 // moving average of the last 5 readings
#define SCALE_STALE_MS 2000UL       // no reading for 2 s = scale not working

struct ScaleChannel {
  const char *name;
  const char *key;      // flash key prefix
  HX711 hx;
  long offset;
  float factor;         // raw counts per gram (0 = not calibrated)
  float buf[SCALE_AVG];
  uint8_t n;
  uint8_t idx;
  float grams;
  unsigned long lastReadMs;
};

static ScaleChannel hopperScale = {"hopper", "hop"};
static ScaleChannel trayScale = {"tray", "tray"};
static Preferences scalePrefs;

static void scaleLoad(ScaleChannel &s) {
  char k[16];
  snprintf(k, sizeof(k), "%sO", s.key);
  s.offset = scalePrefs.getLong(k, 0);
  snprintf(k, sizeof(k), "%sF", s.key);
  s.factor = scalePrefs.getFloat(k, 0);
}

static void scaleSave(ScaleChannel &s) {
  char k[16];
  snprintf(k, sizeof(k), "%sO", s.key);
  scalePrefs.putLong(k, s.offset);
  snprintf(k, sizeof(k), "%sF", s.key);
  scalePrefs.putFloat(k, s.factor);
}

void scalesBegin() {
  scalePrefs.begin("scales", false);
  hopperScale.hx.begin(HOPPER_DOUT_PIN, HOPPER_SCK_PIN);
  trayScale.hx.begin(TRAY_DOUT_PIN, TRAY_SCK_PIN);
  scaleLoad(hopperScale);
  scaleLoad(trayScale);
  Serial.printf("[scale] hopper %s, tray %s\n",
                hopperScale.factor != 0 ? "calibrated" : "NOT calibrated",
                trayScale.factor != 0 ? "calibrated" : "NOT calibrated");
}

// Non-blocking: only reads when the HX711 has a new value
static void scaleUpdate(ScaleChannel &s) {
  if (!s.hx.is_ready()) return;
  long raw = s.hx.read();
  if (s.factor == 0) {
    s.lastReadMs = millis();
    return;
  }
  s.buf[s.idx] = (raw - s.offset) / s.factor;
  s.idx = (s.idx + 1) % SCALE_AVG;
  if (s.n < SCALE_AVG) s.n++;
  float sum = 0;
  for (uint8_t i = 0; i < s.n; i++) sum += s.buf[i];
  s.grams = sum / s.n;
  s.lastReadMs = millis();
}

void scalesLoop() {
  scaleUpdate(hopperScale);
  scaleUpdate(trayScale);
}

// true when the scale is calibrated, answering, and has a value
static bool scaleRead(ScaleChannel &s, float &grams) {
  if (s.factor == 0 || s.n == 0) return false;
  if (millis() - s.lastReadMs > SCALE_STALE_MS) return false;
  grams = s.grams;
  return true;
}

bool hopperGrams(float &g) { return scaleRead(hopperScale, g); }
bool trayGrams(float &g) { return scaleRead(trayScale, g); }

// ---------------- Calibration from the Serial Monitor ----------------
// tare hopper        (empty hopper)  -> sets zero
// cal hopper 1000    (put 1000 g on) -> sets the factor
// scales             -> prints both readings
// Same with "tray".

static long scaleAverageRaw(ScaleChannel &s, uint8_t times) {
  long sum = 0;
  uint8_t got = 0;
  unsigned long start = millis();
  while (got < times && millis() - start < 3000) {
    if (s.hx.is_ready()) {
      sum += s.hx.read();
      got++;
    }
    delay(5);
  }
  return got ? sum / got : 0;
}

static void scaleResetAvg(ScaleChannel &s) {
  s.n = 0;
  s.idx = 0;
}

void scalesHandleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  line.toLowerCase();
  if (line.length() == 0) return;

  ScaleChannel *s = nullptr;
  if (line.indexOf("hopper") >= 0) s = &hopperScale;
  else if (line.indexOf("tray") >= 0) s = &trayScale;

  if (line.startsWith("tare") && s) {
    s->offset = scaleAverageRaw(*s, 20);
    scaleSave(*s);
    scaleResetAvg(*s);
    Serial.printf("[scale] %s zero set (offset %ld)\n", s->name, s->offset);
  } else if (line.startsWith("cal") && s) {
    float known = line.substring(line.lastIndexOf(' ') + 1).toFloat();
    if (known <= 0) {
      Serial.println("[scale] Usage: cal hopper 1000");
      return;
    }
    long raw = scaleAverageRaw(*s, 20);
    s->factor = (raw - s->offset) / known;
    if (s->factor == 0) s->factor = 1;
    scaleSave(*s);
    scaleResetAvg(*s);
    Serial.printf("[scale] %s factor %.3f counts/g\n", s->name, s->factor);
  } else if (line.startsWith("scales")) {
    float g;
    Serial.printf("[scale] hopper: %s", hopperGrams(g) ? String(g, 1).c_str() : "--");
    Serial.printf(" g | tray: %s g\n", trayGrams(g) ? String(g, 1).c_str() : "--");
  } else {
    Serial.println("[scale] Commands: tare hopper | cal hopper 1000 | tare tray | cal tray 500 | scales");
  }
}
