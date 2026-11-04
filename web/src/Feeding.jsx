import { useEffect, useMemo, useState } from "react";
import Icon from "./icons";
import { useSettings } from "./settings";
import {
  requestFeedNow,
  cancelFeedNow,
  saveSchedule,
  saveSettings,
  registerPen,
  finishPen,
  subscribeToBatches,
} from "./dataService";
import {
  useFeedingConfig,
  useFeedCommand,
  useFeedLog,
  nextFeeding,
  formatHHMM,
  COMMAND_TTL_MS,
  DEFAULT_SETTINGS,
  MIN_AGE_DAYS,
  MAX_AGE_DAYS,
  MAX_PIGS,
  manilaDate,
  penPlan,
  toMinutes,
  formatMass,
} from "./feedingData";
import { formatClock, formatManila } from "./deviceData";
import { dateLocale } from "./i18n";

/* ------------------------------------------------------------------ */
/* Feed now                                                           */
/* ------------------------------------------------------------------ */

function FeedNow({ uid, device, settings, defaultGrams }) {
  const { t } = useSettings();
  const cmd = useFeedCommand();
  const [manualEnabled, setManualEnabled] = useState(true);
  const [confirming, setConfirming] = useState(false);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);

  const now = device.serverNow;
  const maxG = Number(settings.maxGramsPerFeed) || 0;
  // Amount box: starts at the next meal's amount until the user types their own
  const [amount, setAmount] = useState("");
  const [touched, setTouched] = useState(false);
  const shown = touched ? amount : String(Math.round(defaultGrams || 300));
  const g = Math.round(Number(shown));

  let state = null;
  if (cmd) {
    const age = now - (cmd.requestedAt || 0);
    if (cmd.status === "pending" && age > COMMAND_TTL_MS) state = "expired";
    else state = cmd.status;
  }
  const active = state === "pending" || state === "running";

  function validate() {
    if (!Number.isFinite(g) || g < 10) return t("feed.tooLittle");
    if (maxG > 0 && g > maxG) return t("feed.tooMuch", { g: maxG });
    return "";
  }

  async function send() {
    setBusy(true);
    setError("");
    try {
      await requestFeedNow({ grams: g, uid });
      setConfirming(false);
    } catch (e) {
      setError(e.message);
    } finally {
      setBusy(false);
    }
  }

  return (
    <section className="card feed-now">
      <span className="card-label"><Icon name="bowl" size={16} />{t("feed.now")}</span>
      <p className="subtle">{t("feed.nowSub")}</p>

      <div className="pref-row">
        <span className="pref-label">{manualEnabled ? t("feed.on") : t("feed.off")}</span>
        <button
          type="button"
          role="switch"
          aria-checked={manualEnabled}
          aria-label={t("feed.manualToggle")}
          className="switch"
          onClick={() => {
            setManualEnabled((enabled) => !enabled);
            setConfirming(false);
          }}
        >
          <span className="switch-knob" />
        </button>
      </div>

      {device.status === "Offline" && !active && (
        <p className="form-warn">{t("feed.offlineWarn")}</p>
      )}
      {error && <p className="form-error" role="alert">{error}</p>}

      {state && (
        <div className={`feed-status feed-status-${state}`} role="status" aria-live="polite">
          {state === "running" || state === "pending" ? (
            <span className="spinner" aria-hidden="true" />
          ) : (
            <Icon name={state === "done" ? "check" : "warn"} size={18} />
          )}
          <span>
            {state === "pending" && t("feed.st.pending")}
            {state === "running" && t("feed.st.running")}
            {state === "done" && t("feed.st.done")}
            {state === "failed" && t("feed.st.failed", { reason: cmd.error || "—" })}
            {state === "expired" && t("feed.st.expired")}
            {state === "cancelled" && t("feed.st.cancelled")}
          </span>
          {state === "pending" && (
            <button type="button" className="btn-link" onClick={cancelFeedNow}>
              {t("feed.cancel")}
            </button>
          )}
        </div>
      )}

      <div className="field">
        <label htmlFor="feed-grams">{t("feed.amount")}</label>
        <div className="grams-input">
          <input
            id="feed-grams"
            type="number"
            inputMode="numeric"
            min="10"
            max={maxG > 0 ? maxG : undefined}
            step="1"
            value={shown}
            disabled={!manualEnabled || active || confirming}
            onChange={(e) => {
              setTouched(true);
              setAmount(e.target.value);
              setError("");
            }}
          />
          <span>g</span>
        </div>
        <span className="subtle small-note">
          {maxG > 0 ? t("feed.amountHint", { max: maxG }) : t("feed.tooLittle")}
        </span>
      </div>

      {!confirming ? (
        <button
          type="button"
          className="btn btn-primary btn-big"
          disabled={!manualEnabled || active}
          onClick={() => {
            const msg = validate();
            setError(msg);
            if (!msg) setConfirming(true);
          }}
        >
          <Icon name="bowl" size={20} />
          {t("feed.button")}
        </button>
      ) : (
        <div className="confirm-box">
          <strong>{t("feed.confirmG", { g })}</strong>
          <div className="form-actions">
            <button type="button" className="btn btn-ghost" onClick={() => setConfirming(false)}>
              {t("feed.cancel")}
            </button>
            <button type="button" className="btn btn-primary" disabled={busy} onClick={send}>
              {busy && <span className="spinner" aria-hidden="true" />}
              {t("feed.yes")}
            </button>
          </div>
        </div>
      )}
    </section>
  );
}

/* ------------------------------------------------------------------ */
/* Schedule: one AM and one PM time                                   */
/* ------------------------------------------------------------------ */

const SLOT_INFO = {
  am: { label: "feed.am" },
  pm: { label: "feed.pm" },
};

function ScheduleRow({ id, slot, grams, canEdit, busy, onSave }) {
  const { t } = useSettings();
  const [editing, setEditing] = useState(false);
  const info = SLOT_INFO[id];
  const [time, setTime] = useState(slot.time);
  const [error, setError] = useState("");

  async function save() {
    const m = toMinutes(time);
    if (
      m === null ||
      !/^\d{2}:\d{2}$/.test(time) ||
      Number(time.slice(0, 2)) > 23 ||
      Number(time.slice(3)) > 59
    ) {
      return setError(t("feed.badTimeFormat"));
    }
    if (id === "am" ? m >= 720 : m < 720) return setError(t(id === "am" ? "feed.amOnly" : "feed.pmOnly"));
    setError("");
    if (await onSave(id, { time, enabled: slot.enabled })) setEditing(false);
  }

  return (
    <li className={`sched-row ${slot.enabled ? "" : "slot-off"} ${editing ? "sched-editing" : ""}`}>
      <span className={`sched-badge sched-${id}`}>{id.toUpperCase()}</span>
      <div className="sched-main">
        <span className="sched-label">{t(info.label)}</span>
        {editing ? (
          <div className="sched-edit">
            <label className="sr-only" htmlFor={`t-${id}`}>{t("feed.time")}</label>
            <input
              id={`t-${id}`}
              className="sched-time-input"
              type="time"
              step="60"
              value={time}
              onChange={(e) => setTime(e.target.value)}
            />
            <div className="sched-edit-actions">
              <button type="button" className="btn btn-ghost btn-sm" onClick={() => { setEditing(false); setError(""); }}>
                {t("feed.cancel")}
              </button>
              <button type="button" className="btn btn-primary btn-sm" disabled={busy} onClick={save}>
                {busy && <span className="spinner" aria-hidden="true" />}
                {t("feed.saveTime")}
              </button>
            </div>
          </div>
        ) : (
          <span className="sched-time-line">
            <span className="slot-time">{formatHHMM(slot.time)}</span>
            <span className="slot-grams">{grams !== null ? formatMass(grams) : t("feed.noAmount")}</span>
          </span>
        )}
        {error && <p className="form-error" role="alert">{error}</p>}
      </div>
      {!editing && (
        <div className="sched-actions">
          {canEdit ? (
            <>
              <button
                type="button"
                role="switch"
                aria-checked={slot.enabled}
                aria-label={`${t(info.label)} ${t("feed.on")}`}
                className="switch"
                disabled={busy}
                onClick={() => onSave(id, { time: slot.time, enabled: !slot.enabled })}
              >
                <span className="switch-knob" />
              </button>
              <button
                type="button"
                className="btn btn-ghost btn-sm"
                onClick={() => { setTime(slot.time); setError(""); setEditing(true); }}
              >
                <Icon name="sliders" size={16} />
                {t("feed.editTime")}
              </button>
            </>
          ) : (
            <span className={`tag ${slot.enabled ? "tag-green" : "tag-grey"}`}>
              {slot.enabled ? t("feed.on") : t("feed.off")}
            </span>
          )}
        </div>
      )}
    </li>
  );
}

function Schedule({ schedule, plan, slots, synced, canEdit, settings, serverNow }) {
  const { t } = useSettings();
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState("");
  const [error, setError] = useState("");

  const next = nextFeeding(slots, serverNow);
  const grams = plan ? Math.round(plan.perFeedG) : null;
  const max = Number(settings.maxGramsPerFeed) || 0;

  async function saveSlot(id, value) {
    setBusy(true);
    setError("");
    setMsg("");
    try {
      const nextSchedule = { ...schedule, [id]: value };
      await saveSchedule({ am: nextSchedule.am, pm: nextSchedule.pm });
      setMsg(t("feed.saved"));
      return true;
    } catch (e) {
      setError(e.message);
      return false;
    } finally {
      setBusy(false);
    }
  }

  return (
    <section className="card schedule">
      <div className="card-head">
        <span className="card-label"><Icon name="clock" size={16} />{t("feed.schedule")}</span>
        {next && (
          <span className="tag tag-green">
            {t("feed.next", {
              time: `${formatHHMM(next.slot.time)}${next.tomorrow ? ` (${t("feed.tomorrow")})` : ""}`,
            })}
          </span>
        )}
      </div>
      <p className="subtle">{t("feed.schedSub")}</p>

      <ul className="sched-list">
        {["am", "pm"].map((id) => (
          <ScheduleRow
            key={id}
            id={id}
            slot={schedule[id]}
            grams={grams}
            canEdit={canEdit}
            busy={busy}
            onSave={saveSlot}
          />
        ))}
      </ul>

      {!plan && <p className="form-warn">{t("feed.needPigs")}</p>}
      {plan && max > 0 && grams > max && (
        <p className="form-warn">{t("feed.overMax", { g: formatMass(grams), max: formatMass(max) })}</p>
      )}
      {error && <p className="form-error" role="alert">{error}</p>}
      {msg && <p className="form-notice" role="status">{msg}</p>}

      <p className={`sync ${synced ? "sync-ok" : "sync-wait"}`}>
        <Icon name={synced ? "check" : "clock"} size={16} />
        {synced ? t("feed.synced") : t("feed.notSynced")}
      </p>
      {!canEdit && <p className="subtle small-note">{t("pub.editLocked")}</p>}
    </section>
  );
}

/* ------------------------------------------------------------------ */
/* Pig registration: age + number of pigs, Finish when sold           */
/* ------------------------------------------------------------------ */

function useBatches() {
  const [rows, setRows] = useState([]);
  useEffect(() => subscribeToBatches(setRows, () => setRows([])), []);
  return rows;
}

const shortDateOf = (ms, lang) => formatManila(ms, lang).split(",").slice(0, 2).join(",");

// Top box: register age + number of pigs. Locked while pigs are registered.
function RegisterPigs({ pen, schedule, canEdit, uid }) {
  const { t } = useSettings();
  const [age, setAge] = useState("");
  const [count, setCount] = useState(1);
  const [confirming, setConfirming] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [msg, setMsg] = useState("");

  const locked = Boolean(pen);
  const disabled = !canEdit || locked || confirming;
  const ageN = Math.round(Number(age));
  const validAge = age !== "" && Number.isFinite(ageN) && ageN >= MIN_AGE_DAYS && ageN <= MAX_AGE_DAYS;
  const today = manilaDate(Date.now());
  const preview = validAge && !locked ? penPlan({ count, ageDays: ageN, registeredDate: today }, Date.now()) : null;

  useEffect(() => {
    if (locked) setConfirming(false);
  }, [locked]);

  async function register() {
    setBusy(true);
    setError("");
    try {
      await registerPen({ count, ageDays: ageN, uid, registeredDate: today, schedule });
      setConfirming(false);
      setAge("");
      setMsg(t("reg.done"));
    } catch (e) {
      setError(/permission/i.test(e.message) ? t("reg.denied") : e.message);
    } finally {
      setBusy(false);
    }
  }

  return (
    <section className={`card register ${locked ? "register-locked" : ""}`}>
      <span className="card-label"><Icon name="plus" size={16} />{t("reg.registerTitle")}</span>
      <p className="subtle">{t("reg.sub")}</p>

      {locked && (
        <p className="form-notice reg-lock-note">
          <Icon name="lock" size={16} />
          {t("reg.locked")}
        </p>
      )}

      <div className="field">
        <label htmlFor="reg-age">{t("reg.age")}</label>
        <div className="grams-input">
          <input
            id="reg-age"
            type="number"
            inputMode="numeric"
            min={MIN_AGE_DAYS}
            max={MAX_AGE_DAYS}
            step="1"
            placeholder={String(MIN_AGE_DAYS)}
            value={locked ? "" : age}
            disabled={disabled}
            onChange={(e) => {
              setAge(e.target.value);
              setMsg("");
            }}
          />
          <span>{t("reg.daysUnit")}</span>
        </div>
        <span className="subtle small-note">{t("reg.ageHint", { min: MIN_AGE_DAYS, max: MAX_AGE_DAYS })}</span>
      </div>

      <div className="field">
        <span className="field-label" id="reg-count-label">{t("reg.count")}</span>
        <div className="count-picker" role="radiogroup" aria-labelledby="reg-count-label">
          {Array.from({ length: MAX_PIGS }, (_, i) => i + 1).map((n) => (
            <button
              key={n}
              type="button"
              role="radio"
              aria-checked={!locked && count === n}
              className={!locked && count === n ? "on" : ""}
              disabled={disabled}
              onClick={() => setCount(n)}
            >
              {n}
            </button>
          ))}
        </div>
        <span className="subtle small-note">{t("reg.countHint", { max: MAX_PIGS })}</span>
      </div>

      {!locked && age !== "" && !validAge && (
        <p className="form-warn">{t("reg.ageHint", { min: MIN_AGE_DAYS, max: MAX_AGE_DAYS })}</p>
      )}
      {preview && (
        <div className="reg-preview">
          <span>{t("reg.perFeed")}</span>
          <strong>{formatMass(preview.perFeedG)}</strong>
          <span className="subtle">{t("reg.previewDay", { g: formatMass(preview.dailyG), per: formatMass(preview.perPigG) })}</span>
        </div>
      )}

      {error && <p className="form-error" role="alert">{error}</p>}
      {msg && <p className="form-notice" role="status">{msg}</p>}

      {!canEdit ? (
        <p className="subtle small-note">{t("pub.penLocked")}</p>
      ) : confirming ? (
        <div className="confirm-box">
          <strong>{t("reg.confirm", { n: count, age: ageN })}</strong>
          <span className="subtle">{t("reg.confirmText")}</span>
          <div className="form-actions">
            <button type="button" className="btn btn-ghost" onClick={() => setConfirming(false)}>
              {t("feed.cancel")}
            </button>
            <button type="button" className="btn btn-primary" disabled={busy} onClick={register}>
              {busy && <span className="spinner" aria-hidden="true" />}
              {t("reg.yes")}
            </button>
          </div>
        </div>
      ) : (
        <button
          type="button"
          className="btn btn-primary btn-big"
          disabled={locked || !validAge}
          onClick={() => setConfirming(true)}
        >
          <Icon name="plus" size={18} />
          {t("reg.button")}
        </button>
      )}
    </section>
  );
}

// Full-width box: what is registered now, the feed table, Finish and past batches
function RegisteredPigs({ pen, plan, canEdit, uid, aiOn }) {
  const { t, lang } = useSettings();
  const batches = useBatches();
  const [confirming, setConfirming] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [msg, setMsg] = useState("");
  const today = manilaDate(Date.now());

  async function finish() {
    setBusy(true);
    setError("");
    try {
      await finishPen(pen, { uid, ageAtFinish: plan.age, finishedDate: today });
      setConfirming(false);
      setMsg(t("reg.finished"));
    } catch (e) {
      setError(e.message);
    } finally {
      setBusy(false);
    }
  }

  const history = batches.length > 0 && (
    <details className="batch-history">
      <summary>{t("reg.history", { n: batches.length })}</summary>
      <div className="table-wrap">
        <table className="table">
          <thead>
            <tr>
              <th scope="col">{t("reg.col.registered")}</th>
              <th scope="col">{t("reg.col.finished")}</th>
              <th scope="col" className="num">{t("reg.col.pigs")}</th>
              <th scope="col" className="num">{t("reg.col.ageEnd")}</th>
            </tr>
          </thead>
          <tbody>
            {batches.map((b) => (
              <tr key={b.id}>
                <td>{b.registeredAt ? shortDateOf(b.registeredAt, lang) : b.registeredDate}</td>
                <td>{b.finishedAt ? shortDateOf(b.finishedAt, lang) : b.finishedDate}</td>
                <td className="num">{b.count}</td>
                <td className="num">{t("reg.days", { n: b.ageAtFinish })}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </details>
  );

  if (!pen || !plan) {
    return (
      <section className="card pen">
        <span className="card-label"><Icon name="users" size={16} />{t("reg.title")}</span>
        <div className="pen-empty">
          <Icon name="users" size={28} />
          <p className="subtle">{t("reg.none")}</p>
        </div>
        {msg && <p className="form-notice" role="status">{msg}</p>}
        {history}
      </section>
    );
  }

  return (
    <section className="card pen">
      <div className="card-head">
        <span className="card-label"><Icon name="users" size={16} />{t("reg.title")}</span>
        <span className="tag tag-green">{t("reg.registered")}</span>
      </div>

      <div className="pen-summary">
        <div className="pen-fact">
          <span className="metric-label">{t("reg.count")}</span>
          <span className="pen-fact-value">{pen.count}</span>
        </div>
        <div className="pen-fact">
          <span className="metric-label">{t("reg.ageToday")}</span>
          <span className="pen-fact-value">{t("reg.days", { n: plan.age })}</span>
        </div>
        <div className="pen-fact">
          <span className="metric-label">{t("reg.registeredOn")}</span>
          <span className="pen-fact-value pen-fact-small">
            {pen.registeredAt ? shortDateOf(pen.registeredAt, lang) : pen.registeredDate}
          </span>
          <span className="subtle small-note">{t("reg.ageThen", { n: pen.ageDays })}</span>
        </div>
      </div>

      <p className="subtle small-note">{t("reg.ageCounts")}</p>
      <p className="reg-ai">
        <Icon name="cpu" size={16} />
        {aiOn
          ? t("reg.aiLine", {
              stage: plan.stage.name,
              per: formatMass(plan.basePerPigG),
              pct: `${plan.adjustPct > 0 ? "+" : ""}${plan.adjustPct}%`,
            })
          : t("reg.aiOff", { stage: plan.stage.name, per: formatMass(plan.basePerPigG) })}
      </p>
      {plan.overAge && <p className="form-warn">{t("reg.overWarn", { n: MAX_AGE_DAYS })}</p>}

      {error && <p className="form-error" role="alert">{error}</p>}

      {canEdit &&
        (confirming ? (
          <div className="confirm-box confirm-danger">
            <strong>{t("reg.finishQ")}</strong>
            <span className="subtle">{t("reg.finishText")}</span>
            <div className="form-actions">
              <button type="button" className="btn btn-ghost" onClick={() => setConfirming(false)}>
                {t("feed.cancel")}
              </button>
              <button type="button" className="btn btn-danger" disabled={busy} onClick={finish}>
                {busy && <span className="spinner" aria-hidden="true" />}
                {t("reg.finishYes")}
              </button>
            </div>
          </div>
        ) : (
          <div className="form-actions">
            <button
              type="button"
              className="btn btn-ghost btn-danger-ghost"
              onClick={() => {
                setConfirming(true);
                setMsg("");
              }}
            >
              <Icon name="check" size={18} />
              {t("reg.finish")}
            </button>
          </div>
        ))}
      {history}
    </section>
  );
}

/* ------------------------------------------------------------------ */
/* Feeding settings                                                  */
/* ------------------------------------------------------------------ */

const SETTING_FIELDS = [
  { key: "maxGramsPerFeed", label: "Max per feeding", unit: "g", help: "Safety cap for one feeding, scheduled or manual." },
  { key: "gramsPerSecond", label: "Motor speed", unit: "g/s", step: "0.1", help: "About how many grams the motor gives per second. Only used to size the safety stop; the hopper load cells decide when to stop." },
  { key: "motorTimeoutSec", label: "Motor timeout", unit: "s", help: "Stops the motor if the target isn't reached (jam or empty hopper)." },
  { key: "aiEnabled", label: "AI feeding", type: "bool", help: "Random Forest + 5-day leftover rule fine-tune each scheduled feeding. Off = the plain amount (target minus what is left in the tray)." },
  { key: "aiStepPct", label: "AI step", unit: "%", help: "Change after 5 days (10 feedings) of empty or leftover trays. Limit ±20 %." },
  { key: "hopperLowGrams", label: "Hopper low alert", unit: "g", help: "Warn when the feed bin drops below this (needs the hopper load cell)." },
  { key: "heatAlertC", label: "Heat alert", unit: "°C", step: "0.5", help: "Warn about heat stress at or above this temperature." },
  { key: "criticalHeatC", label: "Critical heat", unit: "°C", step: "0.5", help: "Red alert: dangerous heat stress at or above this temperature." },
  { key: "coldAlertC", label: "Cold alert", unit: "°C", step: "0.5", help: "Warn when the pen is below this temperature." },
  { key: "humHighPct", label: "Humidity high", unit: "%", help: "Warn when humidity goes above this." },
  { key: "humLowPct", label: "Humidity low", unit: "%", help: "Warn when humidity goes below this." },
  { key: "dispenseToleranceG", label: "Dispensing tolerance", unit: "g", help: "Pass limit for Feed Dispensing Precision (thesis: 5 g)." },
  { key: "timingToleranceSec", label: "Timing tolerance", unit: "s", help: "Pass limit for Timing Precision." },
];

function FeedingSettings({ settings }) {
  const [draft, setDraft] = useState(null);
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState("");
  const [error, setError] = useState("");
  const values = { ...(draft || settings) };

  async function save() {
    setError("");
    const out = {};
    for (const f of SETTING_FIELDS) {
      const v = values[f.key];
      if (f.type === "bool") {
        out[f.key] = !!v;
      } else {
        const n = Number(v);
        if (!Number.isFinite(n) || n < 0) return setError(`${f.label} must be a number.`);
        out[f.key] = n;
      }
    }
    if (out.aiMinGrams > out.aiMaxGrams) return setError("AI minimum must be less than AI maximum.");
    if (out.aiMaxGrams > out.maxGramsPerFeed) return setError("AI maximum can't be more than Max per feeding.");
    setBusy(true);
    try {
      await saveSettings(out);   // device fields: owner only (database rules)
      setDraft(null);
      setMsg("Settings saved. The feeder picks them up on its next check.");
    } catch (e) {
      setError(e.message);
    } finally {
      setBusy(false);
    }
  }

  return (
    <details className="card advanced">
      <summary className="card-head">
        <span className="card-label"><Icon name="sliders" size={16} />Advanced device settings</span>
      </summary>
      <p className="subtle">
        Motor calibration, limits, AI and test tolerances. The defaults are placeholders: replace
        them with your calibration results and your adviser's values.
      </p>
      <div className="settings-grid">
        {SETTING_FIELDS.map((f) => (
          <div className="setting" key={f.key}>
            {f.type === "bool" ? (
              <div className="pref-row">
                <span className="pref-text">
                  <span className="pref-label" id={`lbl-${f.key}`}>{f.label}</span>
                  <span className="subtle">{f.help}</span>
                </span>
                <button
                  type="button"
                  role="switch"
                  aria-checked={!!values[f.key]}
                  aria-labelledby={`lbl-${f.key}`}
                  className="switch"
                  onClick={() => {
                    setMsg("");
                    setDraft({ ...values, [f.key]: !values[f.key] });
                  }}
                >
                  <span className="switch-knob" />
                </button>
              </div>
            ) : (
              <div className="field">
                <label htmlFor={`set-${f.key}`}>{f.label}</label>
                <div className="grams-input">
                  <input
                    id={`set-${f.key}`}
                    type="number"
                    inputMode="decimal"
                    step={f.step || "1"}
                    min="0"
                    value={values[f.key]}
                    onChange={(e) => {
                      setMsg("");
                      setDraft({ ...values, [f.key]: e.target.value });
                    }}
                  />
                  <span>{f.unit}</span>
                </div>
                <span className="subtle small-note">{f.help}</span>
              </div>
            )}
          </div>
        ))}
      </div>
      {error && <p className="form-error" role="alert">{error}</p>}
      {msg && <p className="form-notice" role="status">{msg}</p>}
      <div className="form-actions">
        {draft && (
          <button type="button" className="btn btn-ghost" onClick={() => setDraft(null)}>
            Cancel
          </button>
        )}
        <button
          type="button"
          className="btn btn-ghost"
          onClick={() => {
            setMsg("");
            setDraft({ ...DEFAULT_SETTINGS });
          }}
        >
          Reset to defaults
        </button>
        <button type="button" className="btn btn-primary" disabled={!draft || busy} onClick={save}>
          {busy && <span className="spinner" aria-hidden="true" />}
          Save settings
        </button>
      </div>
    </details>
  );
}

/* ------------------------------------------------------------------ */
/* Feeding log                                                        */
/* ------------------------------------------------------------------ */

function FeedLog({ rows, loaded }) {
  const { t, lang } = useSettings();
  const dateFormatter = new Intl.DateTimeFormat(dateLocale(lang), {
    timeZone: "Asia/Manila",
    year: "numeric",
    month: "long",
    day: "numeric",
  });
  const fmt = (v) => (typeof v === "number" ? `${Math.round(v * 10) / 10} g` : "—");

  return (
    <section className="card">
      <span className="card-label"><Icon name="chart" size={16} />{t("feed.log")}</span>
      {loaded && rows.length === 0 && <p className="subtle">{t("feed.logNone")}</p>}
      {!loaded && <span className="skeleton" style={{ height: 120 }} />}
      {rows.length > 0 && (
        <div className="table-wrap">
          <table className="table feed-log-table">
            <thead>
              <tr>
                <th scope="col">{t("feed.col.date")}</th>
                <th scope="col" className="feed-log-time">{t("feed.col.time")}</th>
                <th scope="col" className="num">{t("feed.col.amountDispensed")}</th>
              </tr>
            </thead>
            <tbody>
              {rows.map((r) => (
                <tr key={r.id}>
                  <td>{r.startedAt ? dateFormatter.format(new Date(r.startedAt)) : "—"}</td>
                  <td className="feed-log-time">{r.startedAt ? formatClock(r.startedAt, lang) : "—"}</td>
                  <td className="num">{fmt(r.scaleGrams ?? r.dispensedGrams)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </section>
  );
}

/* ------------------------------------------------------------------ */
/* Page                                                               */
/* ------------------------------------------------------------------ */

// Visitors see everything; feeding and changes need an approved account
function SignInToFeed({ signedIn }) {
  const { t } = useSettings();
  return (
    <section className="card feed-now feed-locked">
      <span className="card-label"><Icon name="bowl" size={16} />{t("feed.now")}</span>
      <div className="locked-body">
        <span className="locked-icon" aria-hidden="true"><Icon name="lock" size={22} /></span>
        <div>
          <strong>{signedIn ? t("access.feedTitle") : t("pub.feedTitle")}</strong>
          <p className="subtle">{signedIn ? t("access.feedText") : t("pub.feedText")}</p>
        </div>
      </div>
    </section>
  );
}

export default function Feeding({ uid, canEdit, signedIn, device, onSignIn }) {
  const adjustPct = typeof device.data?.adjustPct === "number" ? device.data.adjustPct : 0;
  const { slots, schedule, pen, plan, settings, synced } = useFeedingConfig(adjustPct);
  const { rows, loaded } = useFeedLog(50);
  const defaultGrams = useMemo(() => {
    const n = nextFeeding(slots, device.serverNow);
    return n && n.slot.grams >= 10 ? Math.min(n.slot.grams, Number(settings.maxGramsPerFeed) || n.slot.grams) : 300;
  }, [slots, device.serverNow, settings.maxGramsPerFeed]);

  return (
    <div className="dashboard page-enter">
      <div className="feed-top">
        <RegisterPigs pen={pen} schedule={schedule} canEdit={canEdit} uid={uid} />
        <Schedule
          schedule={schedule}
          plan={plan}
          slots={slots}
          synced={synced}
          canEdit={canEdit}
          settings={settings}
          serverNow={device.serverNow}
        />
      </div>
      <div className="feed-second">
        <RegisteredPigs pen={pen} plan={plan} canEdit={canEdit} uid={uid} aiOn={!!settings.aiEnabled} />
        {canEdit ? (
          <FeedNow uid={uid} device={device} settings={settings} defaultGrams={defaultGrams} />
        ) : (
          <SignInToFeed signedIn={signedIn} />
        )}
      </div>
      <FeedLog rows={rows} loaded={loaded} />
      {canEdit && <FeedingSettings settings={settings} />}
    </div>
  );
}
