import { useEffect, useState } from "react";
import {
  subscribeToConfig,
  subscribeToFeedCommand,
  subscribeToFeedLog,
} from "./dataService";

// Feed per pig per day by age. The ESP32 uses the same numbers
// (SMARTPIG_ESP32/feed_table.h), so keep both in step if they ever change.
//   1-35 days   : 500 g   (group table: 1-60 days = 0.5 kg)
//   36-140 days : B-MEG Premium chart, one value per day
//   141-180 days: 2500 g  (group table: last row = 2.5 kg)
export const BMEG_FROM = 36;
export const BMEG_TO = 140;
export const BMEG_G = [
  250, 270, 285, 305, 320, 340, 355, 375, 395, 410, 430, 445, 465, 480, 500, 500, 525, 550, 570, 595, 620,
  645, 670, 695, 715, 740, 765, 790, 815, 840, 860, 885, 910, 935, 960, 985, 1005, 1030, 1055, 1080, 1105,
  1130, 1150, 1175, 1200, 1200, 1230, 1260, 1290, 1320, 1345, 1375, 1405, 1435, 1465, 1495, 1525, 1555, 1580,
  1610, 1640, 1670, 1700, 1730, 1760, 1790, 1820, 1845, 1875, 1905, 1935, 1965, 1995, 2025, 2055, 2080, 2110,
  2140, 2170, 2200, 2200, 2210, 2225, 2240, 2250, 2260, 2275, 2290, 2300, 2310, 2325, 2340, 2350, 2360, 2375,
  2390, 2400, 2410, 2425, 2440, 2450, 2460, 2475, 2490, 2500,
];
const LOW_AGE_G = 500;
const HIGH_AGE_G = 2500;
// B-MEG stages: limits for the AI adjustment (g per pig per day)
export const STAGES = [
  { id: 1, name: "Pre-Starter", to: 50, min: 250, max: 500 },
  { id: 2, name: "Starter", to: 80, min: 500, max: 1200 },
  { id: 3, name: "Grower 1", to: 115, min: 1200, max: 2200 },
  { id: 4, name: "Grower 2", to: Infinity, min: 2200, max: 2500 },
];
export const MIN_AGE_DAYS = 36;
export const MAX_AGE_DAYS = 180;
export const MAX_PIGS = 3;
export const FEEDS_PER_DAY = 2; // AM + PM

// Two fixed feeding times. Only the time (and on/off) is set by people;
// the amount comes from the registered pigs.
export const DEFAULT_SCHEDULE = {
  am: { time: "07:00", enabled: true },
  pm: { time: "17:00", enabled: true },
};

export const DEFAULT_SETTINGS = {
  pigType: "finisher",
  pigCount: 0,
  avgWeightKg: 0,
  feedPct: 3,
  maxGramsPerFeed: 4000,
  gramsPerSecond: 20,
  motorTimeoutSec: 90,
  manualCooldownMin: 10,
  observationMin: 30,
  aiEnabled: true,
  aiStepPct: 5,
  aiMinGrams: 300,
  aiMaxGrams: 800,
  hopperLowGrams: 2000,
  heatAlertC: 30,
  criticalHeatC: 35,
  coldAlertC: 18,
  humHighPct: 85,
  humLowPct: 40,
  dispenseToleranceG: 5,
  timingToleranceSec: 60,
};

// Pen climate limits from the settings (used by the alerts, the chips and the graphs)
export function climateLimits(settings = {}) {
  const n = (v, d) => (Number.isFinite(Number(v)) && v !== "" && v !== null ? Number(v) : d);
  return {
    heat: n(settings.heatAlertC, 30),
    critical: n(settings.criticalHeatC, 35),
    cold: n(settings.coldAlertC, 18),
    humHigh: n(settings.humHighPct, 85),
    humLow: n(settings.humLowPct, 40),
  };
}

// A manual command older than this is ignored by the ESP32
export const COMMAND_TTL_MS = 2 * 60 * 1000;

export function useFeedingConfig(adjustPct = 0) {
  const [config, setConfig] = useState(null);
  const [error, setError] = useState(null);
  useEffect(
    () =>
      subscribeToConfig(
        (c) => {
          setConfig(c);
          setError(null);
        },
        (e) => setError(e.message)
      ),
    []
  );
  const settings = { ...DEFAULT_SETTINGS, ...((config && config.settings) || {}) };
  const schedule = readSchedule(config && config.schedule);
  const pen = readPen(config && config.pen);
  const [now, setNow] = useState(Date.now());
  // The pigs' age changes at midnight: refresh once an hour
  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), 3600000);
    return () => clearInterval(id);
  }, []);
  // The feeder ignores its adjustment while AI feeding is off
  const plan = pen ? penPlan(pen, now, settings.aiEnabled ? adjustPct : 0) : null;
  // The AM/PM slots with the amount worked out from the registered pigs.
  // No pigs registered = no scheduled feedings.
  const slots = plan
    ? ["am", "pm"].map((id) => ({ id, ...schedule[id], grams: Math.round(plan.perFeedG) }))
    : [];
  const synced =
    !!config && !!config.updatedAt && config.deviceAck === config.updatedAt;
  return { config, settings, schedule, pen, plan, slots, synced, loaded: config !== null, error };
}

// The saved AM/PM times (older free-form slots are ignored)
export function readSchedule(raw) {
  const out = {};
  for (const id of ["am", "pm"]) {
    const s = raw && raw[id];
    out[id] = {
      time: s && toMinutes(s.time) !== null ? s.time : DEFAULT_SCHEDULE[id].time,
      enabled: s ? s.enabled !== false : DEFAULT_SCHEDULE[id].enabled,
    };
  }
  return out;
}

// The registered pigs, or null when none are registered
export function readPen(raw) {
  if (!raw || !(Number(raw.count) > 0) || !/^\d{4}-\d{2}-\d{2}$/.test(raw.registeredDate || "")) return null;
  return {
    count: Number(raw.count),
    ageDays: Number(raw.ageDays) || 0,
    registeredAt: Number(raw.registeredAt) || 0,
    registeredDate: raw.registeredDate,
    by: raw.by || "",
  };
}

// "YYYY-MM-DD" in the Philippines
export function manilaDate(ms) {
  return new Intl.DateTimeFormat("en-CA", {
    timeZone: "Asia/Manila",
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
  }).format(new Date(ms));
}

// Whole days from date a to date b ("YYYY-MM-DD")
export function daysBetween(a, b) {
  const p = (d) => {
    const [y, m, day] = d.split("-").map(Number);
    return Date.UTC(y, m - 1, day);
  };
  return Math.round((p(b) - p(a)) / 86400000);
}

export function stageOf(age) {
  return STAGES.find((s) => age <= s.to);
}

// Base feed per pig per day (grams) for an age in days
export function basePerPigG(age) {
  if (age < BMEG_FROM) return LOW_AGE_G;
  if (age > BMEG_TO) return HIGH_AGE_G;
  return BMEG_G[age - BMEG_FROM];
}

// With the feeder's AI adjustment (5-day leftover rule), kept inside the stage limits
export function adjustedPerPigG(age, adjustPct = 0) {
  const st = stageOf(age);
  const g = basePerPigG(age) * (1 + (Number(adjustPct) || 0) / 100);
  return Math.min(st.max, Math.max(0.8 * st.min, g));
}

// Everything the feeding amount depends on, for the pigs as they are today.
// adjustPct = the feeder's current AI adjustment (from the live data).
export function penPlan(pen, nowMs, adjustPct = 0) {
  const age = pen.ageDays + Math.max(0, daysBetween(pen.registeredDate, manilaDate(nowMs)));
  const adj = Number(adjustPct) || 0;
  const perPigG = adjustedPerPigG(age, adj);
  const dailyG = perPigG * pen.count;
  return {
    age,
    stage: stageOf(age),
    basePerPigG: basePerPigG(age),
    perPigG,
    adjustPct: adj,
    dailyG,
    perFeedG: dailyG / FEEDS_PER_DAY,
    overAge: age > MAX_AGE_DAYS,
  };
}

export function useFeedCommand() {
  const [cmd, setCmd] = useState(undefined);
  useEffect(() => subscribeToFeedCommand(setCmd, () => setCmd(null)), []);
  return cmd;
}

export function useFeedLog(count = 50) {
  const [rows, setRows] = useState([]);
  const [loaded, setLoaded] = useState(false);
  const [error, setError] = useState(null);
  useEffect(
    () =>
      subscribeToFeedLog(
        count,
        (r) => {
          setRows(r);
          setLoaded(true);
          setError(null);
        },
        (e) => {
          setError(e.message);
          setLoaded(true);
        }
      ),
    [count]
  );
  return { rows, loaded, error };
}

// "HH:MM" -> minutes after midnight
export function toMinutes(hhmm) {
  const m = /^(\d{2}):(\d{2})$/.exec(hhmm || "");
  return m ? Number(m[1]) * 60 + Number(m[2]) : null;
}

// Minutes after midnight right now in the Philippines
export function manilaMinutes(ms) {
  const parts = new Intl.DateTimeFormat("en-GB", {
    timeZone: "Asia/Manila",
    hour: "2-digit",
    minute: "2-digit",
    hour12: false,
  }).formatToParts(new Date(ms));
  const h = Number(parts.find((p) => p.type === "hour").value) % 24;
  const m = Number(parts.find((p) => p.type === "minute").value);
  return h * 60 + m;
}

// Next enabled slot after "now" (wraps to tomorrow)
export function nextFeeding(slots, nowMs) {
  const enabled = slots.filter((s) => s.enabled !== false && toMinutes(s.time) !== null);
  if (enabled.length === 0) return null;
  const now = manilaMinutes(nowMs);
  const later = enabled.find((s) => toMinutes(s.time) > now);
  return later ? { slot: later, tomorrow: false } : { slot: enabled[0], tomorrow: true };
}

// "16:00" -> "4:00 PM"
export function formatHHMM(hhmm) {
  const mins = toMinutes(hhmm);
  if (mins === null) return hhmm || "";
  const h = Math.floor(mins / 60);
  const m = String(mins % 60).padStart(2, "0");
  return `${h % 12 === 0 ? 12 : h % 12}:${m} ${h >= 12 ? "PM" : "AM"}`;
}

// Seconds between the scheduled "HH:MM" and the RTC start "HH:MM:SS"
export function delaySeconds(scheduledTime, rtcStart) {
  const s = /^(\d{2}):(\d{2})$/.exec(scheduledTime || "");
  const r = /^(\d{2}):(\d{2}):(\d{2})$/.exec(rtcStart || "");
  if (!s || !r) return null;
  const a = Number(s[1]) * 3600 + Number(s[2]) * 60;
  let b = Number(r[1]) * 3600 + Number(r[2]) * 60 + Number(r[3]);
  if (b - a < -43200) b += 86400; // crossed midnight
  return b - a;
}

// 2600 -> "2.6 kg", 3750 -> "3.75 kg", 450 -> "450 g"
export function formatMass(g) {
  if (!Number.isFinite(g)) return "—";
  return g >= 1000 ? `${(Math.round(g / 10) / 100).toLocaleString()} kg` : `${Math.round(g)} g`;
}
