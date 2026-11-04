import { useMemo, useState } from "react";
import Icon from "./icons";
import { useHistory } from "./historyData";
import { useFeedLog, useFeedingConfig, delaySeconds } from "./feedingData";

// Measures the thesis testing parameters (Chapter 3/4) from what the
// feeder records. English only.

const RANGES = [
  { id: "24h", label: "24 h", ms: 24 * 3600e3 },
  { id: "7d", label: "7 days", ms: 7 * 24 * 3600e3 },
  { id: "30d", label: "30 days", ms: 30 * 24 * 3600e3 },
  { id: "all", label: "All", ms: Infinity },
];

const r1 = (v) => Math.round(v * 10) / 10;
const pct = (a, b) => (b > 0 ? `${Math.round((a / b) * 100)}%` : "—");

function Metric({ label, value, sub }) {
  return (
    <div className="metric-box">
      <span className="metric-label">{label}</span>
      <span className="metric-value">{value}</span>
      {sub && <span className="metric-sub">{sub}</span>}
    </div>
  );
}

function Param({ title, verdict, children, empty }) {
  return (
    <div className="param">
      <div className="param-head">
        <strong>{title}</strong>
        {verdict && <span className={`tag ${verdict.tone}`}>{verdict.text}</span>}
      </div>
      {empty ? <p className="subtle small-note">{empty}</p> : <div className="metric-row">{children}</div>}
    </div>
  );
}

function toCsv(rows) {
  const esc = (v) => {
    if (v === null || v === undefined) return "";
    const s = String(v);
    return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
  };
  return rows.map((r) => r.map(esc).join(",")).join("\r\n");
}

function download(name, text) {
  const blob = new Blob(["﻿" + text], { type: "text/csv;charset=utf-8" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function manila(ms, opts) {
  return new Intl.DateTimeFormat("en-CA", { timeZone: "Asia/Manila", hour12: false, ...opts }).format(
    new Date(ms)
  );
}

export default function TestResults({ device }) {
  const [range, setRange] = useState("7d");
  const { settings } = useFeedingConfig();
  const { rows: log } = useFeedLog(1000);
  const { rows: history } = useHistory(2000);

  const since = device.serverNow - RANGES.find((r) => r.id === range).ms;
  const tolG = Number(settings.dispenseToleranceG) || 5;
  const tolS = Number(settings.timingToleranceSec) || 60;

  const stats = useMemo(() => {
    const feeds = log.filter((r) => (r.startedAt || 0) >= since);
    const hist = history.filter((r) => (r.recordedAt || 0) >= since);

    // 1. Feed dispensing precision
    const trials = feeds
      .filter((r) => typeof r.targetGrams === "number")
      .map((r) => {
        const measured = typeof r.scaleGrams === "number" ? r.scaleGrams : r.dispensedGrams;
        return typeof measured === "number"
          ? { err: Math.abs(measured - r.targetGrams), byScale: typeof r.scaleGrams === "number" }
          : null;
      })
      .filter(Boolean);
    const disp = {
      n: trials.length,
      within: trials.filter((x) => x.err <= tolG).length,
      mean: trials.length ? trials.reduce((a, x) => a + x.err, 0) / trials.length : 0,
      max: trials.length ? Math.max(...trials.map((x) => x.err)) : 0,
      byScale: trials.filter((x) => x.byScale).length,
    };

    // 2. Timing precision
    const delays = feeds
      .filter((r) => r.source === "schedule")
      .map((r) => delaySeconds(r.scheduledTime, r.rtcStart))
      .filter((d) => d !== null);
    const timing = {
      n: delays.length,
      within: delays.filter((d) => Math.abs(d) <= tolS).length,
      early: delays.filter((d) => d < 0).length,
      mean: delays.length ? delays.reduce((a, d) => a + Math.abs(d), 0) / delays.length : 0,
      max: delays.length ? Math.max(...delays.map((d) => Math.abs(d))) : 0,
      failed: feeds.filter((r) => r.ok === false).length,
    };

    const avg = (arr) => (arr.length ? arr.reduce((a, b) => a + b, 0) / arr.length : 0);

    // 3. Data transmission (sequence numbers, per boot)
    const seqRows = hist.filter((r) => typeof r.seq === "number");
    const groups = {};
    for (const r of seqRows) {
      const k = r.bootCount ?? "x";
      (groups[k] = groups[k] || []).push(r.seq);
    }
    let expected = 0;
    let received = 0;
    for (const seqs of Object.values(groups)) {
      const uniq = new Set(seqs);
      expected += Math.max(...uniq) - Math.min(...uniq) + 1;
      received += uniq.size;
    }
    const lat = hist
      .filter((r) => typeof r.sentAt === "number" && typeof r.recordedAt === "number")
      .map((r) => (r.recordedAt - r.sentAt) / 1000)
      .filter((s) => s > -5 && s < 3600);
    const tx = {
      n: seqRows.length,
      expected,
      received,
      lost: expected - received,
      latN: lat.length,
      latAvg: avg(lat),
      latMax: lat.length ? Math.max(...lat) : 0,
    };

    // 4. Stability
    const boots = new Set(hist.map((r) => r.bootCount).filter((b) => b !== undefined));
    const stab = {
      restarts: boots.size > 0 ? boots.size - 1 : null,
      maxUptime: Math.max(0, ...hist.map((r) => r.uptimeSec || 0)),
      failedFeeds: timing.failed,
      sensorFails: hist.filter((r) => r.sensorOk === false).length,
      readings: hist.length,
    };

    return { feeds, hist, disp, timing, tx, stab };
  }, [log, history, since, tolG, tolS]);

  const live = device.data || {};
  const { disp, timing, tx, stab } = stats;

  function exportFeedings() {
    const header = [
      "Date", "Start time", "Source", "Scheduled time", "RTC start", "Delay (s)",
      "Programmed (g)", "Dispensed, load cell (g)", "Dispensed, scale (g)", "Difference (g)",
      `Within ${tolG} g`, "Leftover (g)", "Consumed (g)", "Next amount set (g)", "Result",
    ];
    const rows = [...stats.feeds]
      .sort((a, b) => a.startedAt - b.startedAt)
      .map((r) => {
        const measured = typeof r.scaleGrams === "number" ? r.scaleGrams : r.dispensedGrams;
        const diff =
          typeof measured === "number" && typeof r.targetGrams === "number" ? r1(measured - r.targetGrams) : "";
        return [
          r.startedAt ? manila(r.startedAt, { year: "numeric", month: "2-digit", day: "2-digit" }) : "",
          r.startedAt ? manila(r.startedAt, { hour: "2-digit", minute: "2-digit", second: "2-digit" }) : "",
          r.source || "",
          r.scheduledTime || "",
          r.rtcStart || "",
          r.source === "schedule" ? delaySeconds(r.scheduledTime, r.rtcStart) ?? "" : "",
          r.targetGrams ?? "",
          r.dispensedGrams ?? "",
          r.scaleGrams ?? "",
          diff,
          diff === "" ? "" : Math.abs(diff) <= tolG ? "Yes" : "No",
          r.leftoverGrams ?? "",
          typeof r.leftoverGrams === "number" && typeof r.dispensedGrams === "number"
            ? r1(r.dispensedGrams - r.leftoverGrams)
            : "",
          r.nextGrams ?? "",
          r.ok === false ? `Failed${r.error ? `: ${r.error}` : ""}` : "OK",
        ];
      });
    download(`smartpig-feedings-${range}.csv`, toCsv([header, ...rows]));
  }

  function exportReadings() {
    const header = [
      "Date", "Time", "Seq", "Boot", "Sent at", "Delay (s)", "Temperature (°C)", "Humidity (%)",
      "Water level", "Sensor OK", "Uptime (s)", "Free heap (bytes)",
    ];
    const rows = [...stats.hist]
      .sort((a, b) => a.recordedAt - b.recordedAt)
      .map((r) => [
        manila(r.recordedAt, { year: "numeric", month: "2-digit", day: "2-digit" }),
        manila(r.recordedAt, { hour: "2-digit", minute: "2-digit", second: "2-digit" }),
        r.seq ?? "",
        r.bootCount ?? "",
        r.sentAt ? manila(r.sentAt, { hour: "2-digit", minute: "2-digit", second: "2-digit" }) : "",
        typeof r.sentAt === "number" ? r1((r.recordedAt - r.sentAt) / 1000) : "",
        r.temperature ?? "",
        r.humidity ?? "",
        r.waterLevel ?? "",
        r.sensorOk === true ? "Yes" : r.sensorOk === false ? "No" : "",
        r.uptimeSec ?? "",
        r.freeHeap ?? "",
      ]);
    download(`smartpig-readings-${range}.csv`, toCsv([header, ...rows]));
  }

  const passTone = (ok) => (ok ? "tag-green" : "tag-amber");

  return (
    <section className="card test-results">
      <div className="card-head">
        <span className="card-label"><Icon name="flask" size={16} />Test results (thesis parameters)</span>
        <div className="segmented small" role="group" aria-label="Date range">
          {RANGES.map((r) => (
            <button
              key={r.id}
              type="button"
              className={range === r.id ? "on" : ""}
              aria-pressed={range === r.id}
              onClick={() => setRange(r.id)}
            >
              {r.label}
            </button>
          ))}
        </div>
      </div>

      <Param
        title="Feed Dispensing Precision"
        verdict={disp.n ? { tone: passTone(disp.within === disp.n), text: `${disp.within} of ${disp.n} within ${tolG} g` } : null}
        empty={disp.n ? null : "No feedings with a measured amount yet."}
      >
        <Metric label="Trials" value={disp.n} sub={`${disp.byScale} measured on the scale`} />
        <Metric label="Within tolerance" value={pct(disp.within, disp.n)} />
        <Metric label="Mean error" value={`${r1(disp.mean)} g`} />
        <Metric label="Largest error" value={`${r1(disp.max)} g`} />
      </Param>

      <Param
        title="Timing Precision"
        verdict={timing.n ? { tone: passTone(timing.within === timing.n), text: `${timing.within} of ${timing.n} within ${tolS} s` } : null}
        empty={timing.n ? null : "No scheduled feedings recorded yet."}
      >
        <Metric label="Scheduled feedings" value={timing.n} />
        <Metric label="Mean delay" value={`${r1(timing.mean)} s`} />
        <Metric label="Largest delay" value={`${r1(timing.max)} s`} />
        <Metric label="Early / failed" value={`${timing.early} / ${timing.failed}`} />
      </Param>

      <Param
        title="Data Transmission Efficiency"
        verdict={tx.expected ? { tone: passTone(tx.lost === 0), text: `${pct(tx.received, tx.expected)} delivered` } : null}
        empty={tx.n ? null : "Waiting for the feeder to send sequence numbers (seq) with each reading."}
      >
        <Metric label="Readings received" value={tx.received} sub={`of ${tx.expected} sent`} />
        <Metric label="Lost" value={tx.lost} />
        <Metric label="Avg delay" value={tx.latN ? `${r1(tx.latAvg)} s` : "—"} sub={tx.latN ? null : "needs sentAt"} />
        <Metric label="Largest delay" value={tx.latN ? `${r1(tx.latMax)} s` : "—"} />
      </Param>

      <Param
        title="System Operation Stability"
        verdict={
          stab.restarts !== null
            ? { tone: passTone(stab.restarts === 0 && stab.failedFeeds === 0), text: `${stab.restarts} restart${stab.restarts === 1 ? "" : "s"}` }
            : null
        }
        empty={stab.readings ? null : "No readings in this range."}
      >
        <Metric label="Restarts" value={stab.restarts ?? "—"} sub={stab.restarts === null ? "needs bootCount" : null} />
        <Metric
          label="Longest uptime"
          value={stab.maxUptime ? `${r1(stab.maxUptime / 3600)} h` : "—"}
          sub={live.resetReason ? `last reset: ${live.resetReason}` : null}
        />
        <Metric label="Sensor failures" value={stab.sensorFails} sub={`of ${stab.readings} readings`} />
        <Metric
          label="Free memory now"
          value={typeof live.freeHeap === "number" ? `${Math.round(live.freeHeap / 1024)} KB` : "—"}
        />
      </Param>

      <div className="form-actions export-row">
        <button type="button" className="btn btn-ghost" onClick={exportReadings} disabled={!stats.hist.length}>
          <Icon name="download" size={18} />
          Readings CSV
        </button>
        <button type="button" className="btn btn-primary" onClick={exportFeedings} disabled={!stats.feeds.length}>
          <Icon name="download" size={18} />
          Feedings CSV (Tables 4.2–4.4)
        </button>
      </div>
    </section>
  );
}
