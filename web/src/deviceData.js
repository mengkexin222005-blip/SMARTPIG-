import { useEffect, useState } from "react";
import {
  subscribeToReadings,
  subscribeToConnection,
  subscribeToServerOffset,
} from "./dataService";
import { dateLocale, translate } from "./i18n";

// The ESP32 writes every 5 s. Three missed writes = Offline.
export const OFFLINE_AFTER_MS = 15000;
// Pen temperature at or above this shows a heat alert. Change it to the
// value your adviser or reference uses for fattener pigs.
export const HEAT_ALERT_C = 30;
export const NO_DATA = "No data";

const MONTHS = [
  "January", "February", "March", "April", "May", "June",
  "July", "August", "September", "October", "November", "December",
];

// Server time -> "October 30, 2026, 3:09:42 PM" (Philippine time)
export function formatManila(ms, lang = "en") {
  const locale = dateLocale(lang);
  const date = new Intl.DateTimeFormat(locale, {
    timeZone: "Asia/Manila",
    month: "long",
    day: "numeric",
    year: "numeric",
  }).format(new Date(ms));

  const time = new Intl.DateTimeFormat(locale, {
    timeZone: "Asia/Manila",
    hour: "numeric",
    minute: "2-digit",
    second: "2-digit",
    hour12: true,
  }).format(new Date(ms));

  return `${date}, ${time}`;
}

// Short clock time for charts and tables, e.g. "3:09 PM"
export function formatClock(ms, lang = "en") {
  return new Intl.DateTimeFormat(dateLocale(lang), {
    timeZone: "Asia/Manila",
    hour: "numeric",
    minute: "2-digit",
    hour12: true,
  }).format(new Date(ms));
}

// RTC text ("2026-10-30", "15:08:59") ->
// { date: "October 30, 2026", time: "3:08:59 PM", ms: <as Manila time> }
// Uses the RTC's own numbers directly, with no timezone conversion.
export function formatRtc(dateStr, timeStr) {
  const d = /^(\d{4})-(\d{2})-(\d{2})$/.exec(dateStr || "");
  const t = /^(\d{2}):(\d{2}):(\d{2})$/.exec(timeStr || "");
  if (!d || !t) return null;

  const year = Number(d[1]);
  const month = Number(d[2]);
  const day = Number(d[3]);
  const hour = Number(t[1]);
  const minute = t[2];
  const second = t[3];

  if (
    month < 1 || month > 12 || day < 1 || day > 31 ||
    hour > 23 || Number(minute) > 59 || Number(second) > 59
  ) {
    return null;
  }

  const suffix = hour >= 12 ? "PM" : "AM";
  const hour12 = hour % 12 === 0 ? 12 : hour % 12;
  // Manila is UTC+8 with no daylight saving
  const ms = Date.UTC(year, month - 1, day, hour - 8, Number(minute), Number(second));

  return {
    date: `${MONTHS[month - 1]} ${day}, ${year}`,
    time: `${hour12}:${minute}:${second} ${suffix}`,
    ms,
  };
}

export function floatState(value) {
  if (value === true) return "Closed";
  if (value === false) return "Open";
  return NO_DATA;
}

// 3976000 -> "1 h 6 min ago" (or the Tagalog version)
export function formatAge(ms, lang = "en") {
  const s = Math.max(0, Math.round(ms / 1000));
  if (s < 5) return translate(lang, "time.justNow");
  if (s < 60) return translate(lang, "time.sAgo", { s });
  const m = Math.floor(s / 60);
  if (m < 60) return translate(lang, "time.mAgo", { m });
  const h = Math.floor(m / 60);
  if (h < 24) return translate(lang, "time.hAgo", { h, m: m % 60 });
  const d = Math.floor(h / 24);
  return d === 1 ? translate(lang, "time.dAgo1") : translate(lang, "time.dAgo", { d });
}

// A length of time without "ago": "45 minutes", "2 h"
export function formatSpan(ms, lang = "en") {
  const m = Math.max(1, Math.round(ms / 60000));
  if (m < 90) return translate(lang, "time.min", { m });
  return translate(lang, "time.h", { h: Math.round(m / 60) });
}

// How many of the 3 tank segments to fill. The ESP32 sends one of:
// "FULL", "HALF FULL", "ALMOST LOW", "BELOW LOW" (see database rules).
export function tankSegments(level) {
  const v = String(level || "").toUpperCase().trim();
  if (v === "FULL") return 3;
  if (v === "HALF FULL") return 2;
  if (v === "ALMOST LOW") return 1;
  if (v === "BELOW LOW") return 0;
  return null;
}

// "bad" = refill now, "warn" = getting low, null = fine or unknown
export function waterAlertLevel(level) {
  const v = String(level || "").toUpperCase().trim();
  if (v === "BELOW LOW") return "bad";
  if (v === "ALMOST LOW") return "warn";
  return null;
}

export function isWaterLow(level) {
  return waterAlertLevel(level) !== null;
}

// "HALF FULL" -> "Half full" / "Kalahati"
export function waterLabel(level, lang = "en") {
  const key = `water.${String(level || "").toUpperCase().trim()}`;
  const text = translate(lang, key);
  if (text !== key) return text;
  const s = String(level);
  return s.charAt(0).toUpperCase() + s.slice(1).toLowerCase();
}

// Status words are stored as English keys; screens translate them.
export const STATUS_KEY = {
  Connected: "status.Connected",
  Offline: "status.Offline",
  "Loading...": "status.Loading",
  [NO_DATA]: "status.NoData",
  "Database Error": "status.DatabaseError",
};

const STATUS_TONE = {
  Connected: "ok",
  Offline: "bad",
  "Database Error": "bad",
  "Loading...": "warn",
  [NO_DATA]: "warn",
};

// One subscription for the whole app; every page reads from this.
export function useDeviceData(heatAlertC = HEAT_ALERT_C, hopperLowGrams = 2000, limits = {}) {
  const [data, setData] = useState(null);
  const [hasLoaded, setHasLoaded] = useState(false);
  const [readError, setReadError] = useState(null);
  const [dbConnected, setDbConnected] = useState(false);
  const [serverOffset, setServerOffset] = useState(0);
  const [now, setNow] = useState(() => Date.now());

  useEffect(() => {
    const stopReadings = subscribeToReadings(
      (value) => {
        setData(value);
        setReadError(null);
        setHasLoaded(true);
      },
      (error) => {
        setReadError(error.message);
        setHasLoaded(true);
      }
    );
    const stopConnection = subscribeToConnection(setDbConnected);
    const stopOffset = subscribeToServerOffset(setServerOffset);
    const timer = setInterval(() => setNow(Date.now()), 1000);

    return () => {
      stopReadings();
      stopConnection();
      stopOffset();
      clearInterval(timer);
    };
  }, []);

  // Use the server-corrected time so a wrong phone clock can't fake "Connected"
  const serverNow = now + serverOffset;
  const lastUpdated =
    data && typeof data.lastUpdated === "number" ? data.lastUpdated : null;
  const ageMs = lastUpdated !== null ? serverNow - lastUpdated : null;

  let status;
  if (readError) status = "Database Error";
  else if (!hasLoaded) status = "Loading...";
  else if (lastUpdated === null) status = NO_DATA;
  else if (ageMs > OFFLINE_AFTER_MS) status = "Offline";
  else status = "Connected";

  const rtc =
    data && data.rtcValid === true ? formatRtc(data.rtcDate, data.rtcTime) : null;

  const temperature =
    data && typeof data.temperature === "number" ? data.temperature : null;
  const humidity =
    data && typeof data.humidity === "number" ? data.humidity : null;
  const waterLevel = data && data.waterLevel ? data.waterLevel : null;
  const hopperGrams =
    data && typeof data.hopperGrams === "number" ? data.hopperGrams : null;

  // Alerts, most serious first. Each has an id (used to avoid repeat
  // notifications) and values the screen fills into the translated text.
  const alerts = [];
  if (status === "Database Error") {
    alerts.push({ id: "dbError", tone: "bad", vars: { error: readError } });
  }
  if (status === "Offline") {
    alerts.push({ id: "offline", tone: "bad", vars: { ageMs } });
  }
  const waterAlert = waterAlertLevel(waterLevel);
  if (waterAlert === "bad") alerts.push({ id: "waterBad", tone: "bad", vars: {} });
  if (waterAlert === "warn") alerts.push({ id: "waterWarn", tone: "warn", vars: {} });
  if (status === "Connected" && hopperGrams !== null) {
    if (hopperGrams <= 0) {
      alerts.push({ id: "hopperEmpty", tone: "bad", vars: {} });
    } else if (hopperLowGrams > 0 && hopperGrams < hopperLowGrams) {
      alerts.push({
        id: "hopperLow",
        tone: "warn",
        vars: { grams: Math.round(hopperGrams), threshold: Math.round(hopperLowGrams) },
      });
    }
  }
  // Pen climate: only from a live reading (an old one is not "now")
  const critical = Number(limits.critical) || 35;
  const cold = Number.isFinite(Number(limits.cold)) ? Number(limits.cold) : 18;
  const humHigh = Number(limits.humHigh) || 85;
  const humLow = Number.isFinite(Number(limits.humLow)) ? Number(limits.humLow) : 40;
  if (status === "Connected" && temperature !== null) {
    const temp = temperature.toFixed(1);
    if (temperature >= critical) {
      alerts.push({ id: "heatCritical", tone: "bad", vars: { temp, limit: critical } });
    } else if (temperature >= heatAlertC) {
      alerts.push({ id: "heat", tone: "warn", vars: { temp, limit: heatAlertC } });
    } else if (temperature < cold) {
      alerts.push({ id: "cold", tone: "warn", vars: { temp, limit: cold } });
    }
  }
  if (status === "Connected" && humidity !== null) {
    const hum = Math.round(humidity);
    if (humidity > humHigh) alerts.push({ id: "humHigh", tone: "warn", vars: { hum, limit: humHigh } });
    else if (humidity < humLow) alerts.push({ id: "humLow", tone: "warn", vars: { hum, limit: humLow } });
  }

  return {
    data,
    hasLoaded,
    readError,
    dbConnected,
    serverNow,
    lastUpdated,
    ageMs,
    status,
    statusKey: STATUS_KEY[status] || "status.NoData",
    tone: STATUS_TONE[status] || "warn",
    isStale: status === "Offline",
    rtc,
    temperature,
    humidity,
    waterLevel,
    alerts,
  };
}

// Turns an alert into translated { title, text }
export function alertText(alert, lang) {
  const vars = { ...alert.vars };
  if (alert.id === "offline" && typeof vars.ageMs === "number") {
    vars.age = formatSpan(vars.ageMs, lang);
  }
  return {
    title: translate(lang, `alert.${alert.id}.title`),
    text: translate(lang, `alert.${alert.id}.text`, vars),
  };
}
