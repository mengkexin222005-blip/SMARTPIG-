import { useEffect, useMemo, useRef, useState } from "react";
import { validNumber } from "./historyData";
import { formatClock } from "./deviceData";

// Round, readable axis ticks (e.g. 20, 21, 22, 23)
function niceTicks(min, max, count) {
  const span = max - min || 1;
  const rough = span / count;
  const pow = Math.pow(10, Math.floor(Math.log10(rough)));
  const step = [1, 2, 2.5, 5, 10].map((m) => m * pow).find((s) => s >= rough) || rough;
  const lo = Math.floor(min / step) * step;
  const hi = Math.ceil(max / step) * step;
  const ticks = [];
  for (let v = lo; v <= hi + step / 2; v += step) ticks.push(Number(v.toFixed(6)));
  return { lo, hi, ticks, step };
}

// Safe limits drawn on the graphs. side "above" = a reading at or above the value is
// out; "below" = under it. tone "bad" (red) or "warn" (amber).
export function tempLimits(L, t) {
  return [
    { value: L.critical, side: "above", tone: "bad", label: t("chart.critical") },
    { value: L.heat, side: "above", tone: "warn", label: t("chart.heat") },
    { value: L.cold, side: "below", tone: "warn", label: t("chart.cold") },
  ];
}
export function humLimits(L, t) {
  return [
    { value: L.humHigh, side: "above", tone: "warn", label: t("chart.humid") },
    { value: L.humLow, side: "below", tone: "warn", label: t("chart.dry") },
  ];
}

// The most serious limit a reading breaks, or null
export function brokenLimit(v, limits = []) {
  let hit = null;
  for (const l of limits) {
    if (!Number.isFinite(l.value)) continue;
    const out = l.side === "above" ? v >= l.value : v < l.value;
    if (out && (!hit || (l.tone === "bad" && hit.tone !== "bad"))) hit = l;
  }
  return hit;
}

function useWidth() {
  const ref = useRef(null);
  const [width, setWidth] = useState(0);
  useEffect(() => {
    const el = ref.current;
    if (!el) return undefined;
    setWidth(el.clientWidth);
    if (typeof ResizeObserver === "undefined") return undefined;
    const ro = new ResizeObserver(([entry]) => setWidth(entry.contentRect.width));
    ro.observe(el);
    return () => ro.disconnect();
  }, []);
  return [ref, width];
}

/**
 * Line chart of one reading over time.
 * - full: axes, gridlines, time labels, hover/drag crosshair with tooltip
 * - compact: a small trend line for cards (no axes)
 */
export default function Chart({
  rows,
  field,
  unit,
  tone = "temp",
  lang = "en",
  height = 200,
  compact = false,
  label,
  limits = [],
  area = false,
}) {
  const [wrapRef, width] = useWidth();
  const [hover, setHover] = useState(null);

  const points = useMemo(
    () =>
      rows
        .map((r) => ({ t: r.recordedAt, v: validNumber(r, field) }))
        .filter((p) => typeof p.t === "number"),
    [rows, field]
  );
  const valid = points.filter((p) => p.v !== null);

  const m = compact
    ? { top: 6, right: 6, bottom: 6, left: 6 }
    : { top: 12, right: 12, bottom: 26, left: 40 };
  const W = Math.max(0, width);
  const H = height;
  const innerW = Math.max(1, W - m.left - m.right);
  const innerH = Math.max(1, H - m.top - m.bottom);

  if (valid.length < 2) {
    return <div ref={wrapRef} className="chart-empty" style={{ height: compact ? height : undefined }} />;
  }

  const dMin = Math.min(...valid.map((p) => p.v));
  const dMax = Math.max(...valid.map((p) => p.v));
  // Show a limit line when it is near the readings (or crossed), so a far-away
  // limit doesn't squash the line flat
  const reach = Math.max(4, (dMax - dMin) * 0.6);
  const shown = limits.filter(
    (l) => Number.isFinite(l.value) && l.value >= dMin - reach && l.value <= dMax + reach
  );
  const vMin = Math.min(dMin, ...shown.map((l) => l.value));
  const vMax = Math.max(dMax, ...shown.map((l) => l.value));
  const pad = vMin === vMax ? 1 : 0;
  const { lo, hi, ticks } = compact
    ? { lo: vMin - pad, hi: vMax + pad, ticks: [] }
    : niceTicks(vMin - pad, vMax + pad, 4);

  const t0 = points[0].t;
  const t1 = points[points.length - 1].t;
  // Full chart: x = real time. Small card trend: readings evenly spaced, so a long
  // gap (feeder off for hours) doesn't squeeze the recent readings into a spike.
  const order = new Map(points.map((p, i) => [p.t, i]));
  const x = compact
    ? (t) => m.left + ((order.get(t) ?? 0) / Math.max(1, points.length - 1)) * innerW
    : (t) => m.left + ((t - t0) / (t1 - t0 || 1)) * innerW;
  const y = (v) => m.top + innerH - ((v - lo) / (hi - lo || 1)) * innerH;

  // A failed reading breaks the line instead of being drawn as zero
  let d = "";
  let penDown = false;
  for (const p of points) {
    if (p.v === null) {
      penDown = false;
      continue;
    }
    d += `${penDown ? "L" : "M"}${x(p.t).toFixed(1)} ${y(p.v).toFixed(1)} `;
    penDown = true;
  }

  // Area under the line (soft gradient), only for continuous stretches
  let areaD = "";
  if (area) {
    const base = (m.top + innerH).toFixed(1);
    let run = [];
    const flush = () => {
      if (run.length > 1) {
        areaD += `M${x(run[0].t).toFixed(1)} ${base} `;
        for (const p of run) areaD += `L${x(p.t).toFixed(1)} ${y(p.v).toFixed(1)} `;
        areaD += `L${x(run[run.length - 1].t).toFixed(1)} ${base} Z `;
      }
      run = [];
    };
    for (const p of points) {
      if (p.v === null) flush();
      else run.push(p);
    }
    flush();
  }
  const gradId = `chart-fill-${tone}-${compact ? "s" : "l"}`;
  const last = valid[valid.length - 1];

  // 3–4 time labels along the bottom
  const xTicks = [];
  if (!compact && W > 0) {
    const n = W < 420 ? 3 : 4;
    for (let i = 0; i < n; i++) xTicks.push(t0 + ((t1 - t0) * i) / (n - 1));
  }

  function pickFromClientX(clientX, rect) {
    const px = clientX - rect.left;
    const t = t0 + ((px - m.left) / innerW) * (t1 - t0);
    let best = null;
    for (const p of valid) {
      if (!best || Math.abs(p.t - t) < Math.abs(best.t - t)) best = p;
    }
    return best;
  }

  function onPointer(e) {
    if (compact) return;
    const rect = e.currentTarget.getBoundingClientRect();
    setHover(pickFromClientX(e.clientX, rect));
  }

  function onKey(e) {
    if (compact) return;
    if (e.key !== "ArrowLeft" && e.key !== "ArrowRight") return;
    e.preventDefault();
    const idx = hover ? valid.findIndex((p) => p.t === hover.t) : valid.length;
    const next = e.key === "ArrowLeft" ? Math.max(0, idx - 1) : Math.min(valid.length - 1, idx + 1);
    setHover(valid[next]);
  }

  const decimals = 1;
  const hx = hover ? x(hover.t) : null;
  const hy = hover ? y(hover.v) : null;
  const tipLeft = hover ? Math.min(Math.max(hx, 70), W - 70) : 0;

  return (
    <div ref={wrapRef} className={`chart chart-${tone} ${compact ? "chart-compact" : ""}`}>
      {W > 0 && (
        <svg
          width={W}
          height={H}
          viewBox={`0 0 ${W} ${H}`}
          role="img"
          aria-label={label}
          tabIndex={compact ? -1 : 0}
          onPointerMove={onPointer}
          onPointerDown={onPointer}
          onPointerLeave={() => setHover(null)}
          onBlur={() => setHover(null)}
          onKeyDown={onKey}
          className="chart-svg"
        >
          {!compact &&
            ticks.map((v) => (
              <g key={v}>
                <line
                  className="chart-grid"
                  x1={m.left}
                  x2={W - m.right}
                  y1={y(v)}
                  y2={y(v)}
                />
                <text className="chart-axis" x={m.left - 8} y={y(v)} dy="0.32em" textAnchor="end">
                  {Number.isInteger(v) ? v : v.toFixed(1)}
                </text>
              </g>
            ))}
          {!compact &&
            xTicks.map((t, i) => (
              <text
                key={t}
                className="chart-axis"
                x={x(t)}
                y={H - 6}
                textAnchor={i === 0 ? "start" : i === xTicks.length - 1 ? "end" : "middle"}
              >
                {formatClock(t, lang)}
              </text>
            ))}

          {shown.map((l) => (
            <g key={`${l.side}${l.value}`} className={`chart-limit chart-limit-${l.tone}`}>
              <line x1={m.left} x2={W - m.right} y1={y(l.value)} y2={y(l.value)} />
              {!compact && (
                <text x={m.left + 6} y={y(l.value)} dy="-0.45em" textAnchor="start">
                  {l.label} {l.value}
                  {unit === "%" ? "%" : ` ${unit}`}
                </text>
              )}
            </g>
          ))}

          {area && (
            <>
              <defs>
                <linearGradient id={gradId} x1="0" y1="0" x2="0" y2="1">
                  <stop offset="0%" className="chart-fill-top" />
                  <stop offset="100%" className="chart-fill-bottom" />
                </linearGradient>
              </defs>
              <path className="chart-area" d={areaD} fill={`url(#${gradId})`} />
            </>
          )}
          <path className="chart-line" d={d} />

          {/* Readings outside the safe limits */}
          {valid.map((p) => {
            const hit = brokenLimit(p.v, shown);
            return hit ? (
              <circle
                key={`o${p.t}`}
                className={`chart-out chart-out-${hit.tone}`}
                cx={x(p.t)}
                cy={y(p.v)}
                r={compact ? 2.2 : 3}
              />
            ) : null;
          })}

          {/* Latest reading */}
          <circle className="chart-dot" cx={x(last.t)} cy={y(last.v)} r={compact ? 3.5 : 4} />

          {hover && (
            <g className="chart-hover">
              <line x1={hx} x2={hx} y1={m.top} y2={m.top + innerH} />
              <circle className="chart-dot" cx={hx} cy={hy} r={5} />
            </g>
          )}
        </svg>
      )}
      {hover && (
        <div className="chart-tip" style={{ left: tipLeft }} aria-live="polite">
          <strong>
            {hover.v.toFixed(decimals)} {unit}
          </strong>
          <span>{formatClock(hover.t, lang)}</span>
        </div>
      )}
    </div>
  );
}
