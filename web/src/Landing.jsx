import { useDeviceData, formatAge, waterLabel } from "./deviceData";
import { useSettings } from "./settings";
import { LangSwitch, TroughRim } from "./Login";
import { LogoMark } from "./Logo";
import Icon from "./icons";

// Public home page: what SmartPig is, how it works, and a live strip from the pen.
// "Sign in" opens the login page.

function LiveStrip() {
  const { t, lang } = useSettings();
  const device = useDeviceData();
  const online = device.status === "Connected";
  const has = device.hasLoaded && device.lastUpdated !== null;
  const temp = device.temperature;
  const hum = device.humidity;

  const items = [
    {
      icon: "temp",
      label: t("land.live.temp"),
      value: online && temp !== null ? `${temp.toFixed(1)} °C` : "—",
    },
    {
      icon: "drop",
      label: t("land.live.hum"),
      value: online && hum !== null ? `${Math.round(hum)} %` : "—",
    },
    {
      icon: "drop",
      label: t("land.live.water"),
      value: online && device.waterLevel ? waterLabel(device.waterLevel, lang) : "—",
      water: true,
    },
  ];

  return (
    <section className="land-live" aria-labelledby="land-live-title">
      <div className="land-live-head">
        <h2 id="land-live-title">{t("land.live.title")}</h2>
        <span className={`land-status ${online ? "on" : "off"}`}>
          <span className="dot" aria-hidden="true" />
          {!device.hasLoaded
            ? t("land.live.loading")
            : online
              ? t("land.live.online")
              : t("land.live.offline")}
        </span>
      </div>
      <div className="land-live-grid">
        {items.map((it) => (
          <div key={it.label} className={`land-live-item ${it.water ? "land-live-water" : ""}`}>
            <span className="land-live-ico"><Icon name={it.icon} size={20} /></span>
            <span className="land-live-label">{it.label}</span>
            <strong className="land-live-value">{it.value}</strong>
          </div>
        ))}
      </div>
      <p className="land-live-note">
        {has
          ? t("land.live.updated", { age: formatAge(device.ageMs, lang) })
          : t("land.live.none")}
      </p>
    </section>
  );
}

const STEPS = [
  { icon: "temp", key: "s1" },
  { icon: "cpu", key: "s2" },
  { icon: "chart", key: "s3" },
  { icon: "phone", key: "s4" },
];

const FEATURES = [
  { icon: "clock", key: "f1" },
  { icon: "bowl", key: "f2" },
  { icon: "chart", key: "f3" },
  { icon: "temp", key: "f4" },
  { icon: "bell", key: "f5" },
  { icon: "globe", key: "f6" },
];

export default function Landing({ onSignIn }) {
  const { t } = useSettings();

  return (
    <div className="land">
      <header className="land-top">
        <div className="land-brand">
          <LogoMark size={36} />
          <span className="land-wordmark">
            Smart<span className="land-pig">Pig</span>
          </span>
        </div>
        <div className="land-top-actions">
          <LangSwitch className="land-lang" />
          <button type="button" className="btn btn-primary land-top-signin" onClick={onSignIn}>
            {t("land.signIn")}
          </button>
        </div>
      </header>

      <main>
        <section className="land-hero">
          <div className="land-hero-text">
            <span className="land-badge">
              <Icon name="globe" size={16} />
              {t("land.badge")}
            </span>
            <h1>{t("land.title")}</h1>
            <p className="land-lead">{t("land.lead")}</p>
            <div className="land-cta">
              <button type="button" className="lp-submit land-cta-main" onClick={onSignIn}>
                {t("land.signIn")}
                <svg className="icon" width="20" height="20" viewBox="0 0 24 24" aria-hidden="true">
                  <path d="M5 12h14M13 6l6 6-6 6" />
                </svg>
              </button>
              <a className="land-cta-link" href="#land-how">{t("land.how")}</a>
            </div>
          </div>
          <div className="land-hero-art" aria-hidden="true">
            <div className="land-trough">
              <img src="/brand/piglet.webp" alt="" width="560" height="416" />
              <TroughRim />
            </div>
          </div>
        </section>

        <div className="land-body">
          <LiveStrip />

          <section id="land-how" className="land-section" aria-labelledby="land-how-title">
            <h2 id="land-how-title">{t("land.howTitle")}</h2>
            <p className="land-section-sub">{t("land.howSub")}</p>
            <ol className="land-steps">
              {STEPS.map((s, i) => (
                <li key={s.key} className="land-step">
                  <span className="land-step-num">{i + 1}</span>
                  <span className="land-step-ico"><Icon name={s.icon} size={22} /></span>
                  <strong>{t(`land.${s.key}.title`)}</strong>
                  <span>{t(`land.${s.key}.text`)}</span>
                </li>
              ))}
            </ol>
          </section>

          <section className="land-section" aria-labelledby="land-feat-title">
            <h2 id="land-feat-title">{t("land.featTitle")}</h2>
            <div className="land-features">
              {FEATURES.map((f) => (
                <div key={f.key} className="land-feature">
                  <span className="land-feature-ico"><Icon name={f.icon} size={20} /></span>
                  <strong>{t(`land.${f.key}.title`)}</strong>
                  <span>{t(`land.${f.key}.text`)}</span>
                </div>
              ))}
            </div>
          </section>

          <section className="land-thesis" aria-labelledby="land-thesis-title">
            <span className="land-thesis-kicker">{t("land.thesisKicker")}</span>
            <h2 id="land-thesis-title">{t("land.thesisTitle")}</h2>
            <p>{t("land.thesisText")}</p>
            <button type="button" className="lp-submit land-cta-main" onClick={onSignIn}>
              {t("land.signIn")}
              <svg className="icon" width="20" height="20" viewBox="0 0 24 24" aria-hidden="true">
                <path d="M5 12h14M13 6l6 6-6 6" />
              </svg>
            </button>
          </section>
        </div>
      </main>

      <footer className="land-foot">
        <span>© {new Date().getFullYear()} SmartPig</span>
        <span>{t("land.foot")}</span>
      </footer>
    </div>
  );
}
