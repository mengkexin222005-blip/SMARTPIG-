import Chart, { tempLimits, humLimits, brokenLimit } from "./Chart";
import { useFeedingConfig, climateLimits } from "./feedingData";
import Icon from "./icons";
import { useSettings } from "./settings";
import { useHistory, validNumber, RECORD_COUNT } from "./historyData";
import { formatClock, waterLabel } from "./deviceData";

function Stats({ rows, field, unit, t }) {
  const values = rows.map((r) => validNumber(r, field)).filter((v) => v !== null);
  if (values.length === 0) return null;
  const min = Math.min(...values);
  const max = Math.max(...values);
  const avg = values.reduce((a, b) => a + b, 0) / values.length;
  const fmt = (v) => `${v.toFixed(1)} ${unit}`;
  return (
    <dl className="stats">
      <div><dt>{t("hist.min")}</dt><dd>{fmt(min)}</dd></div>
      <div><dt>{t("hist.avg")}</dt><dd>{fmt(avg)}</dd></div>
      <div><dt>{t("hist.max")}</dt><dd>{fmt(max)}</dd></div>
    </dl>
  );
}

// "3 readings at or above 30 °C (heat) · last at 2:10 PM", most serious first
export function LimitNotice({ rows, field, unit, limits, t, lang }) {
  const counts = new Map();
  for (const r of rows) {
    const v = validNumber(r, field);
    if (v === null) continue;
    const hit = brokenLimit(v, limits);
    if (!hit) continue;
    const c = counts.get(hit) || { n: 0, last: 0 };
    c.n += 1;
    c.last = Math.max(c.last, r.recordedAt || 0);
    counts.set(hit, c);
  }
  const u = unit === "%" ? "%" : ` ${unit}`;
  if (counts.size === 0) {
    return <p className="chart-notice chart-notice-ok"><Icon name="check" size={16} />{t("chart.allSafe")}</p>;
  }
  return [...counts.entries()]
    .sort(([a], [b]) => (a.tone === "bad" ? -1 : 0) - (b.tone === "bad" ? -1 : 0))
    .map(([l, c]) => (
      <p key={`${l.side}${l.value}`} className={`chart-notice chart-notice-${l.tone}`}>
        <Icon name="warn" size={16} />
        {t(l.side === "above" ? "chart.outAbove" : "chart.outBelow", {
          n: c.n,
          limit: `${l.value}${u}`,
          name: l.label.toLowerCase(),
          time: c.last ? formatClock(c.last, lang) : "—",
        })}
      </p>
    ));
}

function ChartCard({ title, icon, rows, field, unit, tone, t, lang, limits }) {
  const values = rows.map((r) => validNumber(r, field)).filter((v) => v !== null);
  const latest = values.length ? values[values.length - 1] : null;
  return (
    <section className="card chart-card">
      <div className="chart-head">
        <span className="card-label"><Icon name={icon} size={16} />{title}</span>
        {latest !== null && (
          <span className="chart-latest">
            {latest.toFixed(1)}
            <span className="unit"> {unit}</span>
          </span>
        )}
      </div>
      {values.length < 2 ? (
        <p className="subtle">{t("hist.notEnough")}</p>
      ) : (
        <>
          <Chart
            rows={rows}
            field={field}
            unit={unit}
            tone={tone}
            lang={lang}
            height={200}
            label={`${title}, ${t("hist.lastN", { n: rows.length })}`}
            limits={limits}
          />
          <LimitNotice rows={rows} field={field} unit={unit} limits={limits} t={t} lang={lang} />
          <Stats rows={rows} field={field} unit={unit} t={t} />
        </>
      )}
    </section>
  );
}

export default function History() {
  const { t, lang } = useSettings();
  const { rows, hasLoaded, error } = useHistory(RECORD_COUNT);
  const { settings } = useFeedingConfig();
  const L = climateLimits(settings);
  const newestFirst = [...rows].reverse().slice(0, 10);

  if (error) {
    return (
      <section className="card">
        <p className="error">{t("hist.error", { error })}</p>
      </section>
    );
  }
  if (!hasLoaded) {
    return (
      <div className="grid-2">
        <section className="card"><span className="skeleton" style={{ height: 240 }} /></section>
        <section className="card"><span className="skeleton" style={{ height: 240 }} /></section>
      </div>
    );
  }
  if (rows.length === 0) {
    return (
      <section className="card">
        <p className="subtle">{t("hist.none")}</p>
      </section>
    );
  }

  return (
    <>
      <p className="subtle">
        {t("hist.lastN", { n: rows.length })} · {t("hist.chartHint")}
      </p>
      <div className="grid-2">
        <ChartCard
          title={t("ov.temperature")}
          icon="temp"
          rows={rows}
          field="temperature"
          unit="°C"
          tone="temp"
          t={t}
          lang={lang}
          limits={tempLimits(L, t)}
        />
        <ChartCard
          title={t("ov.humidity")}
          icon="drop"
          rows={rows}
          field="humidity"
          unit="%"
          tone="hum"
          t={t}
          lang={lang}
          limits={humLimits(L, t)}
        />
      </div>

      <section className="card">
        <span className="card-label">{t("hist.latest10")}</span>
        <div className="table-wrap">
          <table className="table history-table">
            <thead>
              <tr>
                <th scope="col">{t("hist.time")}</th>
                <th scope="col" className="num">{t("hist.temp")}</th>
                <th scope="col" className="num">{t("hist.hum")}</th>
                <th scope="col">{t("hist.water")}</th>
              </tr>
            </thead>
            <tbody>
              {newestFirst.map((r) => {
                const temp = validNumber(r, "temperature");
                const hum = validNumber(r, "humidity");
                return (
                  <tr key={r.id}>
                    <td>{formatClock(r.recordedAt, lang)}</td>
                    <td className="num">{temp !== null ? `${temp.toFixed(1)} °C` : t("ov.noData")}</td>
                    <td className="num">{hum !== null ? `${hum.toFixed(1)} %` : t("ov.noData")}</td>
                    <td>{r.waterLevel ? waterLabel(r.waterLevel, lang) : t("ov.noData")}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      </section>
    </>
  );
}
