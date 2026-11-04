import Icon from "./icons";
import Chart, { tempLimits, humLimits } from "./Chart";
import { useSettings } from "./settings";
import { useHistory, validNumber } from "./historyData";
import { TroughRim } from "./Login";
import { useFeedingConfig, useFeedLog, nextFeeding, formatHHMM, climateLimits } from "./feedingData";
import {
  formatManila,
  formatAge,
  formatSpan,
  tankSegments,
  isWaterLow,
  waterLabel,
  alertText,
} from "./deviceData";

function Skeleton({ w = "60%", h = 34 }) {
  return <span className="skeleton" style={{ width: w, height: h }} aria-hidden="true" />;
}

function Trend({ rows, field, unit, tone, lang, t, current, limits }) {
  if (rows.length < 2) return null;
  const span = rows[rows.length - 1].recordedAt - rows[0].recordedAt;
  // Same readings the line uses (good sensor readings only), plus the value shown above
  const vals = rows.map((r) => validNumber(r, field)).filter((v) => v !== null);
  if (typeof current === "number") vals.push(current);
  const lo = vals.length ? Math.min(...vals) : null;
  const hi = vals.length ? Math.max(...vals) : null;
  return (
    <div className="trend">
      <Chart rows={rows} field={field} unit={unit} tone={tone} lang={lang} height={48} compact limits={limits} />
      <div className="trend-foot">
        <span className="trend-label">{t("time.lastSpan", { span: formatSpan(span, lang) })}</span>
        {lo !== null && (
          <span className="trend-range">
            {t("ov.low")} <strong>{lo.toFixed(1)}</strong> · {t("ov.high")} <strong>{hi.toFixed(1)}</strong>
          </span>
        )}
      </div>
    </div>
  );
}

// Small colored chip that says what a reading means
function tempState(c, L) {
  if (c === null) return null;
  if (c >= L.critical) return ["bad", "ov.st.critical"];
  if (c >= L.heat) return ["bad", "ov.st.hot"];
  if (c >= L.heat - 3) return ["warn", "ov.st.warm"];
  if (c < L.cold) return ["warn", "ov.st.cold"];
  return ["ok", "ov.st.comfy"];
}
function humState(h, L) {
  if (h === null) return null;
  if (h > L.humHigh) return ["warn", "ov.st.humid"];
  if (h < L.humLow) return ["warn", "ov.st.dry"];
  return ["ok", "ov.st.normal"];
}
const WATER_HINT = {
  FULL: ["ok", "ov.wh.full"],
  "HALF FULL": ["ok", "ov.wh.half"],
  "ALMOST LOW": ["warn", "ov.wh.almost"],
  "BELOW LOW": ["bad", "ov.wh.below"],
};

function Chip({ state, t }) {
  if (!state) return null;
  return <span className={`state-chip state-${state[0]}`}>{t(state[1])}</span>;
}

// After this long without data, the cards say "No live data" instead of the old number
const NO_LIVE_AFTER_MS = 10 * 60 * 1000;

function MetricHead({ icon, kind, label, state, stale, noLive, t }) {
  return (
    <div className="metric-head">
      <span className={`metric-ico metric-ico-${kind}`}><Icon name={icon} size={18} /></span>
      <span className="metric-name">{label}</span>
      {noLive ? (
        <span className="state-chip state-bad">{t("ov.offlineChip")}</span>
      ) : stale ? (
        <span className="state-chip state-old">{t("ov.lastReading")}</span>
      ) : (
        <Chip state={state} t={t} />
      )}
    </div>
  );
}

function NoLive({ last, age, t }) {
  return (
    <div className="no-live-body">
      <span className="value muted">{t("ov.noLive")}</span>
      {last && <span className="no-live-last">{t("ov.lastWas", { value: last, age })}</span>}
    </div>
  );
}

function FeedTile({ icon, label, value, sub, tone = "", pending }) {
  return (
    <div className={`feed-tile ${tone ? `feed-tile-${tone}` : ""} ${pending ? "feed-tile-pending" : ""}`}>
      <span className="feed-tile-ico"><Icon name={icon} size={18} /></span>
      <span className="feed-tile-label">{label}</span>
      <span className="feed-tile-value">{value}</span>
      {sub && <span className="feed-tile-sub">{sub}</span>}
    </div>
  );
}

// Greeting by the time of day in the Philippines
function greetingKey(ms) {
  const hour = Number(
    new Intl.DateTimeFormat("en-US", { timeZone: "Asia/Manila", hour: "numeric", hour12: false })
      .format(new Date(ms))
  ) % 24;
  if (hour < 12) return "hero.morning";
  if (hour < 18) return "hero.afternoon";
  return "hero.evening";
}

// The main page for everyone (visitors and signed-in users)
export default function Overview({ device, name, onOpenFeeding }) {
  const { t, lang } = useSettings();
  const { slots, settings, pen } = useFeedingConfig();
  const L = climateLimits(settings);
  const { rows: feedLog } = useFeedLog(1);
  const { rows } = useHistory();
  const {
    status, tone, isStale, lastUpdated, ageMs, hasLoaded,
    temperature, humidity, waterLevel, alerts,
  } = device;

  const segments = tankSegments(waterLevel);
  const waterHint = WATER_HINT[String(waterLevel || "").toUpperCase()] || null;
  // Offline longer than a short Wi-Fi blip: don't show old numbers as if they were live
  const noLive = isStale && ageMs !== null && ageMs > NO_LIVE_AFTER_MS;
  const lastAge = ageMs !== null ? formatAge(ageMs, lang) : "";
  const waterLow = waterLevel && isWaterLow(waterLevel);
  const loading = !hasLoaded;

  const headline =
    status === "Connected"
      ? t("ov.online")
      : status === "Offline"
      ? t("ov.offline")
      : t(device.statusKey);

  return (
    <div className="dashboard page-enter">
      {/* Welcome card: greeting, status and the piglet */}
      <section className={`card hero hero-${tone}`}>
        <div className="hero-text">
          <h2 className="hero-title">
            {t(greetingKey(device.serverNow))}
            {name ? `, ${name}!` : "!"}
          </h2>
          <p className="hero-sub">
            {status === "Offline" ? t("hero.subOffline") : t("hero.subOk")}
          </p>
          <div className="hero-status">
            <span className={`pill pill-${tone}`}>
              <span className={`dot ${tone === "ok" ? "live" : ""}`} />
              {loading ? t("status.Loading") : headline}
            </span>
            <span className="subtle">
              {t("ov.lastUpdate")}:{" "}
              {lastUpdated !== null ? formatManila(lastUpdated, lang) : t("ov.noData")}
              {ageMs !== null && ` · ${formatAge(ageMs, lang)}`}
            </span>
          </div>
        </div>
        <div className="hero-art" aria-hidden="true">
          <span className="hero-bubble">
            {loading ? t("hero.wait") : status === "Connected" ? t("hero.ok") : t("hero.offline")}
          </span>
          <img className="hero-piglet" src="/brand/piglet.webp" alt="" width="560" height="416" />
          <TroughRim />
        </div>
      </section>

      {/* Alerts */}
      {alerts.length > 0 && (
        <section className="card alerts" aria-label={t("ov.alerts")}>
          <div className="card-head">
            <span className="card-label"><Icon name="bell" size={16} />{t("ov.alerts")}</span>
            <span className="count-chip">{alerts.length}</span>
          </div>
          {alerts.map((a) => {
            const { title, text } = alertText(a, lang);
            return (
              <div key={a.id} className={`alert alert-${a.tone}`}>
                <span className="alert-ico"><Icon name="warn" size={18} /></span>
                <div>
                  <strong>{title}</strong>
                  {text && <span>{text}</span>}
                </div>
              </div>
            );
          })}
        </section>
      )}

      {/* Main readings */}
      <div className="grid-3">
        <section className={`card metric metric-temp ${isStale ? "is-old" : ""} ${noLive ? "no-live" : ""}`}>
          <MetricHead
            icon="temp" kind="temp" label={t("ov.temperature")} t={t}
            stale={isStale && temperature !== null} noLive={noLive}
            state={tempState(temperature, L)}
          />
          {loading ? (
            <Skeleton />
          ) : noLive ? (
            <NoLive last={temperature !== null ? `${temperature.toFixed(1)} °C` : null} age={lastAge} t={t} />
          ) : temperature !== null ? (
            <span className="value" key={temperature}>
              {temperature.toFixed(1)}
              <span className="unit">°C</span>
            </span>
          ) : (
            <span className="value muted">{t("ov.noData")}</span>
          )}
          {!noLive && (
            <Trend rows={rows} field="temperature" unit="°C" tone="temp" lang={lang} t={t} current={temperature} limits={tempLimits(L, t)} />
          )}
        </section>

        <section className={`card metric metric-hum ${isStale ? "is-old" : ""} ${noLive ? "no-live" : ""}`}>
          <MetricHead
            icon="drop" kind="hum" label={t("ov.humidity")} t={t}
            stale={isStale && humidity !== null} noLive={noLive}
            state={humState(humidity, L)}
          />
          {loading ? (
            <Skeleton />
          ) : noLive ? (
            <NoLive last={humidity !== null ? `${humidity.toFixed(1)} %` : null} age={lastAge} t={t} />
          ) : humidity !== null ? (
            <span className="value" key={humidity}>
              {humidity.toFixed(1)}
              <span className="unit">%</span>
            </span>
          ) : (
            <span className="value muted">{t("ov.noData")}</span>
          )}
          {!noLive && (
            <Trend rows={rows} field="humidity" unit="%" tone="hum" lang={lang} t={t} current={humidity} limits={humLimits(L, t)} />
          )}
        </section>

        <section className={`card metric water ${isStale ? "is-old" : ""} ${noLive ? "no-live" : ""}`}>
          <MetricHead
            icon="drop" kind="water" label={t("ov.water")} t={t}
            stale={isStale && Boolean(waterLevel)} noLive={noLive}
            state={waterHint ? [waterHint[0], `ov.wl.${waterHint[0]}`] : null}
          />
          <div className="water-body">
            <div className={`tank ${waterLow && !noLive ? "tank-low" : ""}`} aria-hidden="true">
              {[2, 1, 0].map((i) => (
                <div
                  key={i}
                  className={`tank-seg ${!noLive && segments !== null && i < segments ? "filled" : ""}`}
                />
              ))}
            </div>
            <div className="water-text">
              {loading ? (
                <Skeleton w="80%" />
              ) : noLive ? (
                <NoLive last={waterLevel ? waterLabel(waterLevel, lang) : null} age={lastAge} t={t} />
              ) : (
                <>
                  <span className={`value ${waterLevel ? "" : "muted"}`}>
                    {waterLevel ? waterLabel(waterLevel, lang) : t("ov.noData")}
                  </span>
                  {waterHint && <span className="water-hint">{t(waterHint[1])}</span>}
                  {segments !== null && (
                    <div className="water-bar" aria-hidden="true">
                      <span style={{ width: `${(segments / 3) * 100}%` }} />
                    </div>
                  )}
                </>
              )}
            </div>
          </div>
        </section>
      </div>

      {/* Feeding */}
      <section className="card feed-summary">
        <div className="card-head">
          <span className="card-label"><Icon name="bowl" size={16} />{t("ov.feeding")}</span>
          {onOpenFeeding && (
            <button type="button" className="btn btn-ghost btn-sm feed-open" onClick={onOpenFeeding}>
              {t("ov.openFeeding")}
              <svg className="icon" width="16" height="16" viewBox="0 0 24 24" aria-hidden="true">
                <path d="M5 12h14M13 6l6 6-6 6" />
              </svg>
            </button>
          )}
        </div>
        {(() => {
          const next = nextFeeding(slots, device.serverNow);
          // Only feedings of the pigs registered now: after Finish (pigs sold) there is none
          const penStart = pen ? pen.registeredAt || Date.parse(`${pen.registeredDate}T00:00:00+08:00`) : null;
          const last = feedLog[0];
          const previous =
            pen && last && typeof last.startedAt === "number" && last.startedAt >= penStart ? last : null;
          const data = device.data || {};
          const lowAt = Number(settings.hopperLowGrams) || 0;
          const hopper = typeof data.hopperGrams === "number" ? data.hopperGrams : null;
          const kg = (g) => (g >= 1000 ? `${(g / 1000).toFixed(g >= 10000 ? 1 : 2)} kg` : `${Math.round(g)} g`);
          return (
            <div className="feed-tiles">
              <FeedTile
                icon="clock"
                tone="main"
                label={t("ov.next")}
                pending={!next}
                value={next ? formatHHMM(next.slot.time) : "—"}
                sub={next ? `${next.slot.grams} g${next.tomorrow ? ` · ${t("feed.tomorrow")}` : ""}` : t("ov.noSchedule")}
              />
              <FeedTile
                icon="check"
                label={t("ov.previousFeed")}
                pending={!previous}
                value={
                  previous
                    ? `${Math.round(previous.scaleGrams ?? previous.dispensedGrams ?? previous.targetGrams ?? 0)} g`
                    : "—"
                }
                sub={
                  previous && typeof previous.startedAt === "number"
                    ? formatAge(device.serverNow - previous.startedAt, lang)
                    : pen
                      ? t("ov.noFeedYet")
                      : t("ov.noPigsFeed")
                }
              />
              <FeedTile
                icon="bowl"
                tone={hopper !== null && lowAt && hopper < lowAt ? "warn" : ""}
                label={t("ov.hopper")}
                pending={hopper === null}
                value={hopper !== null ? kg(hopper) : "—"}
                sub={hopper === null ? t("ov.needsScale") : lowAt && hopper < lowAt ? t("ov.refillHopper") : t("ov.hopperOk")}
              />
            </div>
          );
        })()}
      </section>
    </div>
  );
}
