import Icon from "./icons";
import TestResults from "./TestResults";
import { useFeedingConfig, useFeedLog, formatHHMM } from "./feedingData";
import {
  NO_DATA,
  OFFLINE_AFTER_MS,
  HEAT_ALERT_C,
  formatManila,
  formatAge,
  floatState,
} from "./deviceData";

function Row({ label, children }) {
  return (
    <div className="row">
      <span className="row-label">{label}</span>
      <span className="row-value">{children}</span>
    </div>
  );
}

function Tag({ tone = "grey", children }) {
  return <span className={`tag tag-${tone}`}>{children}</span>;
}

// Signed seconds, e.g. "+3 s", "−12 s"
function formatDrift(ms) {
  const s = Math.round(ms / 1000);
  if (s === 0) return "0 s";
  return `${s > 0 ? "+" : "−"}${Math.abs(s)} s`;
}

// Device health and raw sensor details (owner only)
export default function System({ device }) {
  const { slots, settings, synced } = useFeedingConfig();
  const { rows: feedLog } = useFeedLog(5);
  const lastFeed = feedLog[0];
  const {
    data, status, tone, dbConnected, lastUpdated, ageMs, rtc,
    temperature, humidity,
  } = device;

  const floats = [
    ["Top", floatState(data && data.waterTop)],
    ["Middle", floatState(data && data.waterMiddle)],
    ["Bottom", floatState(data && data.waterBottom)],
  ];

  // RTC vs. the time the reading reached Firebase (approximate, includes
  // the upload delay). Relates to the schedule-accuracy objective.
  const drift = rtc && lastUpdated !== null ? rtc.ms - lastUpdated : null;
  const driftTone =
    drift === null ? "grey" : Math.abs(drift) <= 60000 ? "green" : "amber";

  const sensorOk = data && typeof data.sensorOk === "boolean" ? data.sensorOk : null;

  return (
    <div className="dashboard page-enter">
      <div className="grid-2">
        <section className="card list-card">
          <span className="card-label"><Icon name="wifi" size={16} />Connection</span>
          <Row label="Device status">
            <Tag tone={tone === "ok" ? "green" : tone === "bad" ? "red" : "amber"}>
              {status}
            </Tag>
          </Row>
          <Row label="Last update">
            {lastUpdated !== null ? formatManila(lastUpdated) : NO_DATA}
          </Row>
          <Row label="Data age">{ageMs !== null ? formatAge(ageMs) : NO_DATA}</Row>
          <Row label="Offline after">{OFFLINE_AFTER_MS / 1000} s without data</Row>
          <Row label="Dashboard ↔ Firebase">
            <Tag tone={dbConnected ? "green" : "red"}>
              {dbConnected ? "Connected" : "Disconnected"}
            </Tag>
          </Row>
          <Row label="LoRa link">
            <Tag>Not reported</Tag>
          </Row>
        </section>

        <section className="card list-card">
          <span className="card-label"><Icon name="clock" size={16} />Real-time clock (DS3231)</span>
          <Row label="RTC valid">
            <Tag tone={data && data.rtcValid === true ? "green" : "red"}>
              {data && data.rtcValid === true ? "Yes" : "No"}
            </Tag>
          </Row>
          <Row label="RTC time">{rtc ? rtc.time : NO_DATA}</Row>
          <Row label="RTC date">{rtc ? rtc.date : NO_DATA}</Row>
          <Row label="Drift vs. server">
            {drift !== null ? <Tag tone={driftTone}>{formatDrift(drift)}</Tag> : NO_DATA}
          </Row>
          <p className="subtle small-note">
            Drift compares the RTC with the time the reading reached Firebase,
            so it also includes the upload delay.
          </p>
        </section>

        <section className="card list-card">
          <span className="card-label"><Icon name="temp" size={16} />Temperature and humidity sensor</span>
          <Row label="Sensor reading">
            {sensorOk === null ? (
              <Tag>Not reported</Tag>
            ) : (
              <Tag tone={sensorOk ? "green" : "red"}>{sensorOk ? "OK" : "Failed"}</Tag>
            )}
          </Row>
          <Row label="Temperature">
            {temperature !== null ? `${temperature.toFixed(1)} °C` : NO_DATA}
          </Row>
          <Row label="Humidity">
            {humidity !== null ? `${humidity.toFixed(1)} %` : NO_DATA}
          </Row>
          <Row label="Heat alert at">{HEAT_ALERT_C} °C</Row>
        </section>

        <section className="card list-card">
          <span className="card-label"><Icon name="drop" size={16} />Water float switches</span>
          {floats.map(([name, state]) => (
            <Row key={name} label={name}>
              <Tag tone={state === "Closed" ? "blue" : state === "Open" ? "amber" : "grey"}>
                {state}
              </Tag>
            </Row>
          ))}
          <Row label="Reported level">{(data && data.waterLevel) || NO_DATA}</Row>
        </section>

        <section className="card list-card">
          <span className="card-label"><Icon name="bowl" size={16} />Feeder and load cells</span>
          <Row label="Hopper load cell">
            {typeof data?.hopperGrams === "number" ? `${Math.round(data.hopperGrams)} g` : <Tag>Pending setup</Tag>}
          </Row>
          <Row label="Tray load cell">
            {typeof data?.trayGrams === "number" ? `${Math.round(data.trayGrams)} g` : <Tag>Pending setup</Tag>}
          </Row>
          <Row label="Feeding schedule">
            {slots.length
              ? `${slots.filter((x) => x.enabled !== false).map((x) => formatHHMM(x.time)).join(", ") || "All off"}`
              : <Tag>Not set</Tag>}
          </Row>
          <Row label="Schedule on device">
            <Tag tone={synced ? "green" : "amber"}>{synced ? "Up to date" : "Waiting"}</Tag>
          </Row>
          <Row label="Last feeding">
            {lastFeed
              ? `${lastFeed.dispensedGrams ?? "?"} g · ${lastFeed.ok === false ? "failed" : "ok"}`
              : <Tag>None yet</Tag>}
          </Row>
          <Row label="AI adjustment">
            {settings.aiEnabled
              ? (typeof data?.adjustPct === "number"
                  ? `${data.adjustPct > 0 ? "+" : ""}${data.adjustPct}% · last: ${lastFeed?.decidedBy === "ai" ? "Random Forest" : lastFeed?.decidedBy === "formula" ? "formula" : "—"}`
                  : <Tag>Waiting</Tag>)
              : <Tag>AI off</Tag>}
          </Row>
        </section>

        <section className="card raw">
          <span className="card-label"><Icon name="cpu" size={16} />Raw device data</span>
          <details>
            <summary>Show the latest record from /smartpig</summary>
            <pre>{data ? JSON.stringify(data, null, 2) : NO_DATA}</pre>
          </details>
        </section>
      </div>
      <TestResults device={device} />
    </div>
  );
}
