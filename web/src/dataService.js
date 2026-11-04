import {
  ref,
  onValue,
  query,
  orderByChild,
  limitToLast,
  set,
  get,
  push,
  update,
  serverTimestamp,
  increment,
} from "firebase/database";
import { db } from "./firebase";

// Each subscribe function returns an "unsubscribe" function for cleanup.

/* ---------------- Device data ---------------- */

export function subscribeToReadings(onData, onError) {
  return onValue(
    ref(db, "smartpig"),
    (snap) => onData(snap.val()),
    (err) => onError(err)
  );
}

export function subscribeToConnection(onChange) {
  return onValue(ref(db, ".info/connected"), (snap) =>
    onChange(snap.val() === true)
  );
}

// Difference between Firebase's server clock and this device's clock
export function subscribeToServerOffset(onChange) {
  return onValue(ref(db, ".info/serverTimeOffset"), (snap) =>
    onChange(snap.val() || 0)
  );
}

// Last N history records, oldest first
export function subscribeToHistory(count, onData, onError) {
  const q = query(
    ref(db, "smartpigHistory"),
    orderByChild("recordedAt"),
    limitToLast(count)
  );

  return onValue(
    q,
    (snap) => {
      const rows = [];
      snap.forEach((child) => {
        rows.push({ id: child.key, ...child.val() });
      });
      rows.sort((a, b) => a.recordedAt - b.recordedAt);
      onData(rows);
    },
    (err) => onError(err)
  );
}

/* ---------------- Accounts ----------------
   Anyone can view the dashboard without signing in. Each account has a profile
   at /users/{uid}:
   { name, email, role, createdAt, lastSignInAt, lastSeenAt, signInCount, lastDevice }
   role: "user"     = Farmer: full access (feed, schedule, register pigs,
                            system, users, device settings)
         "disabled" = access removed (view only)
         "admin" / "pending" = older roles, treated as full access            */

export const ROLES = ["user", "disabled"];
// Every account that signs up gets full access ("user" = Farmer). "admin" and
// "pending" are older roles that count as full access too. "disabled" = removed.
export const canControl = (role) => role === "admin" || role === "user";

export function subscribeToProfile(uid, onData, onError) {
  return onValue(
    ref(db, `users/${uid}`),
    (snap) => onData(snap.val()),
    (err) => onError(err)
  );
}

// Approved accounts only (enforced by the database rules)
export function subscribeToUsers(onData, onError) {
  return onValue(
    ref(db, "users"),
    (snap) => {
      const rows = [];
      snap.forEach((child) => {
        rows.push({ uid: child.key, ...child.val() });
      });
      rows.sort((a, b) => (b.lastSeenAt || 0) - (a.lastSeenAt || 0));
      onData(rows);
    },
    (err) => onError(err)
  );
}

// "Android · Chrome", "Windows · Edge", ... from the browser's user agent
export function describeDevice(ua = typeof navigator !== "undefined" ? navigator.userAgent : "") {
  const os = /Android/i.test(ua) ? "Android"
    : /iPhone|iPad|iPod/i.test(ua) ? "iPhone/iPad"
    : /Windows/i.test(ua) ? "Windows"
    : /Mac OS X|Macintosh/i.test(ua) ? "Mac"
    : /Linux/i.test(ua) ? "Linux"
    : "Other";
  const browser = /Edg\//.test(ua) ? "Edge"
    : /OPR\/|Opera/.test(ua) ? "Opera"
    : /SamsungBrowser/.test(ua) ? "Samsung Internet"
    : /Firefox\//.test(ua) ? "Firefox"
    : /Chrome\//.test(ua) ? "Chrome"
    : /Safari\//.test(ua) ? "Safari"
    : "Browser";
  return `${os} · ${browser}`;
}

// Called right after a sign-in or sign-up: saves who signed in, when and from what device
export async function recordSignIn(user, name) {
  const profileRef = ref(db, `users/${user.uid}`);
  const snap = await get(profileRef);
  const existing = snap.val() || {};
  await update(profileRef, {
    name: (name || existing.name || (user.email || "").split("@")[0]).trim(),
    email: (user.email || "").toLowerCase(),
    createdAt: existing.createdAt || serverTimestamp(),
    lastSignInAt: serverTimestamp(),
    lastSeenAt: serverTimestamp(),
    signInCount: increment(1),
    lastDevice: describeDevice(),
    // New accounts get full access right away. Older "pending" accounts move up too.
    ...(!existing.role || existing.role === "pending" ? { role: "user" } : {}),
  });
}

// Called when the dashboard opens with someone already signed in
export async function recordVisit(user) {
  const profileRef = ref(db, `users/${user.uid}`);
  const snap = await get(profileRef);
  if (!snap.exists() || !snap.val().role || snap.val().role === "pending") return recordSignIn(user);
  return update(profileRef, { lastSeenAt: serverTimestamp(), lastDevice: describeDevice() });
}

// Farmers only (enforced by the database rules)
export function setUserRole(uid, role) {
  return update(ref(db, `users/${uid}`), { role });
}

export function updateProfileName(uid, name) {
  return update(ref(db, `users/${uid}`), { name: name.trim() });
}

/* ---------------- Feeding ----------------
   smartpigConfig            schedule + feeding settings (farmers edit, ESP32 reads)
   smartpigCommands/feedNow  one manual-feed request at a time (web writes, ESP32 updates status)
   smartpigFeedLog           one record per feeding (ESP32 writes; farmers may add scaleGrams)
   See docs/feeding-protocol.md for the exact fields. */

export function subscribeToConfig(onData, onError) {
  return onValue(
    ref(db, "smartpigConfig"),
    (snap) => onData(snap.val() || {}),
    (err) => onError(err)
  );
}

// Replace the whole schedule ({am, pm}) in one write
export function saveSchedule(slots) {
  return update(ref(db, "smartpigConfig"), {
    schedule: slots,
    updatedAt: Date.now(),
  });
}

// Saves only the given settings, one field at a time ("settings/pigType", ...).
// The database rules decide per field who may change it:
//   every field -> any farmer (all accounts have full access)
export function saveSettings(partial) {
  const patch = { updatedAt: Date.now() };
  for (const [k, v] of Object.entries(partial)) patch[`settings/${k}`] = v;
  return update(ref(db, "smartpigConfig"), patch);
}

// Register the pigs in the pen. The database refuses this while pigs are
// already registered: press Finish first.
export function registerPen({ count, ageDays, uid, registeredDate, schedule }) {
  return update(ref(db, "smartpigConfig"), {
    // Save the AM/PM times too, so the feeder has them even if nobody pressed Save
    ...(schedule ? { schedule: { am: schedule.am, pm: schedule.pm } } : {}),
    pen: {
      count,
      ageDays,
      registeredAt: Date.now(),
      registeredDate,
      by: uid,
    },
    updatedAt: Date.now(),
  });
}

// Finish = the pigs were sold or moved. Saves the batch to history and clears
// the pen in one write, so a new batch can be registered.
export function finishPen(pen, { uid, ageAtFinish, finishedDate }) {
  const key = push(ref(db, "smartpigBatches")).key;
  return update(ref(db), {
    [`smartpigBatches/${key}`]: {
      count: pen.count,
      ageDays: pen.ageDays,
      registeredAt: pen.registeredAt,
      registeredDate: pen.registeredDate,
      ageAtFinish,
      finishedAt: Date.now(),
      finishedDate,
      by: uid,
    },
    "smartpigConfig/pen": null,
    "smartpigConfig/updatedAt": Date.now(),
  });
}

// Finished batches, newest first
export function subscribeToBatches(onData, onError) {
  return onValue(
    query(ref(db, "smartpigBatches"), orderByChild("finishedAt"), limitToLast(20)),
    (snap) => {
      const rows = [];
      snap.forEach((child) => {
        rows.push({ id: child.key, ...child.val() });
      });
      rows.sort((a, b) => (b.finishedAt || 0) - (a.finishedAt || 0));
      onData(rows);
    },
    (err) => onError(err)
  );
}

export function requestFeedNow({ grams, uid }) {
  return set(ref(db, "smartpigCommands/feedNow"), {
    id: `c_${Date.now().toString(36)}`,
    grams,
    by: uid,
    requestedAt: Date.now(),
    status: "pending",
  });
}

export function cancelFeedNow() {
  return update(ref(db, "smartpigCommands/feedNow"), { status: "cancelled" });
}

export function subscribeToFeedCommand(onData, onError) {
  return onValue(
    ref(db, "smartpigCommands/feedNow"),
    (snap) => onData(snap.val()),
    (err) => onError(err)
  );
}

// Last N feedings, newest first
export function subscribeToFeedLog(count, onData, onError) {
  const q = query(ref(db, "smartpigFeedLog"), orderByChild("startedAt"), limitToLast(count));
  return onValue(
    q,
    (snap) => {
      const rows = [];
      snap.forEach((child) => {
        rows.push({ id: child.key, ...child.val() });
      });
      rows.sort((a, b) => (b.startedAt || 0) - (a.startedAt || 0));
      onData(rows);
    },
    (err) => onError(err)
  );
}

// A farmer enters the weight measured on the calibrated digital scale (Table 4.2)
export function setScaleGrams(logId, grams) {
  return update(ref(db, `smartpigFeedLog/${logId}`), {
    scaleGrams: grams === null ? null : Number(grams),
  });
}
