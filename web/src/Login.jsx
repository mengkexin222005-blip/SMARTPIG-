import { useEffect, useRef, useState } from "react";
import { signIn, signUp, resetPassword, authErrorKey } from "./authService";
import { useSettings } from "./settings";
import { LANGS } from "./i18n";
import Icon from "./icons";

export function LangSwitch({ className = "" }) {
  const { lang, setLang } = useSettings();
  return (
    <div className={`segmented small ${className}`} role="group" aria-label="Language / Wika">
      {LANGS.map((l) => (
        <button
          key={l.id}
          type="button"
          className={lang === l.id ? "on" : ""}
          aria-pressed={lang === l.id}
          onClick={() => setLang(l.id)}
        >
          {l.id === "en" ? "EN" : "TL"}
        </button>
      ))}
    </div>
  );
}

// The steel trough rim the piglet peeks over, with a few feed pellets
export function TroughRim() {
  return (
    <svg className="lp-rim" viewBox="0 0 620 40" preserveAspectRatio="none" aria-hidden="true">
      <defs>
        <linearGradient id="lp-steel" x1="0" y1="0" x2="1" y2="0">
          <stop offset="0%" stopColor="#6b7680" />
          <stop offset="12%" stopColor="#b9c3cb" />
          <stop offset="30%" stopColor="#ffffff" />
          <stop offset="50%" stopColor="#c4ccd3" />
          <stop offset="70%" stopColor="#eef1f3" />
          <stop offset="88%" stopColor="#9aa5af" />
          <stop offset="100%" stopColor="#5d6871" />
        </linearGradient>
        <radialGradient id="lp-pellet" cx="35%" cy="30%" r="70%">
          <stop offset="0%" stopColor="#f0b868" />
          <stop offset="100%" stopColor="#8a5222" />
        </radialGradient>
      </defs>
      <rect x="0" y="12" width="620" height="28" rx="14" fill="url(#lp-steel)" />
      <rect x="14" y="16" width="592" height="3" rx="1.5" fill="#ffffff" opacity="0.9" />
      {[[40, 10], [56, 7], [72, 11], [90, 9], [530, 10], [546, 7], [562, 11], [580, 9]].map(([cx, cy]) => (
        <ellipse key={cx} cx={cx} cy={cy} rx="8" ry="5" fill="url(#lp-pellet)" />
      ))}
    </svg>
  );
}

export default function Login({ onBack }) {
  const { t } = useSettings();
  const innerRef = useRef(null);
  const [scale, setScale] = useState(1);
  const [mode, setMode] = useState("signin"); // "signin" | "signup"
  const isSignUp = mode === "signup";
  const [name, setName] = useState("");
  const [email, setEmail] = useState("");
  const [password, setPassword] = useState("");
  const [remember, setRemember] = useState(true);
  const [showPassword, setShowPassword] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");

  useEffect(() => {
    const element = innerRef.current;
    if (!element) return undefined;

    function fitToViewport() {
      const width = element.offsetWidth;
      const height = element.offsetHeight;
      const availableWidth = window.innerWidth - 32;
      const availableHeight = window.innerHeight - 24;
      const nextScale = Math.min(
        1,
        availableWidth / width,
        availableHeight / height
      );
      setScale((current) => (Math.abs(current - nextScale) < 0.001 ? current : nextScale));
    }

    const observer = new ResizeObserver(fitToViewport);
    observer.observe(element);
    window.addEventListener("resize", fitToViewport);
    window.visualViewport?.addEventListener("resize", fitToViewport);
    fitToViewport();

    return () => {
      observer.disconnect();
      window.removeEventListener("resize", fitToViewport);
      window.visualViewport?.removeEventListener("resize", fitToViewport);
    };
  }, []);

  async function handleSubmit(e) {
    e.preventDefault();
    setError("");
    setNotice("");
    if (isSignUp && !name.trim()) {
      setError(t("signup.needName"));
      return;
    }
    if (!email.trim() || !password) {
      setError(t("login.needBoth"));
      return;
    }
    if (isSignUp && password.length < 6) {
      setError(t("auth.weak"));
      return;
    }
    setBusy(true);
    try {
      if (isSignUp) await signUp(name, email, password, remember);
      else await signIn(email, password, remember);
      // App.jsx notices the sign-in and shows the dashboard
    } catch (err) {
      setError(t(authErrorKey(err)));
      setBusy(false);
    }
  }

  async function handleForgot() {
    setError("");
    setNotice("");
    if (!email.trim()) {
      setError(t("login.needEmail"));
      return;
    }
    try {
      await resetPassword(email);
      setNotice(t("login.resetSent"));
    } catch (err) {
      setError(t(authErrorKey(err)));
    }
  }

  return (
    <div className="lp">
      {onBack && (
        <button type="button" className="lp-back" onClick={onBack}>
          <svg className="icon" width="18" height="18" viewBox="0 0 24 24" aria-hidden="true">
            <path d="M19 12H5M11 18l-6-6 6-6" />
          </svg>
          {t("pub.back")}
        </button>
      )}
      <LangSwitch className="lp-lang" />

      <main
        ref={innerRef}
        className="lp-inner"
        style={{ "--lp-scale": scale }}
      >
        <div className="lp-brand">
          <span className="lp-wordmark">
            Smart<span className="lp-wordmark-pig">Pig</span>
          </span>
          <span className="lp-badge">{t("login.badge")}</span>
        </div>

        <div className="lp-hero" aria-hidden="true">
          <img className="lp-piglet" src="/brand/piglet.webp" alt="" width="560" height="416" />
          <span className="lp-bubble">{t("login.bubble")}</span>
        </div>

        <form className="lp-card" onSubmit={handleSubmit} noValidate>
          <TroughRim />
          <span className="lp-glow" aria-hidden="true" />

          <div className="lp-head">
            <h1>{isSignUp ? t("signup.heading") : t("login.heading")}</h1>
            <p>{isSignUp ? t("signup.sub") : t("login.sub")}</p>
          </div>

          {isSignUp && (
            <div className="lp-field">
              <label htmlFor="name">{t("signup.name")}</label>
              <div className="lp-input">
                <Icon name="user" size={18} />
                <input
                  id="name"
                  type="text"
                  autoComplete="name"
                  value={name}
                  onChange={(e) => setName(e.target.value)}
                  placeholder={t("signup.namePh")}
                />
              </div>
            </div>
          )}

          <div className="lp-field">
            <label htmlFor="email">{t("login.email")}</label>
            <div className="lp-input">
              <svg className="icon" width="18" height="18" viewBox="0 0 24 24" aria-hidden="true">
                <rect x="3" y="5" width="18" height="14" rx="2" />
                <path d="m3 7 9 6 9-6" />
              </svg>
              <input
                id="email"
                type="email"
                autoComplete="email"
                inputMode="email"
                value={email}
                onChange={(e) => setEmail(e.target.value)}
                placeholder="you@farm.com"
              />
            </div>
          </div>

          <div className="lp-field">
            <div className="lp-label-row">
              <label htmlFor="password">{t("login.password")}</label>
              {!isSignUp && (
                <button type="button" className="lp-link" onClick={handleForgot}>
                  {t("login.forgot")}
                </button>
              )}
            </div>
            <div className="lp-input">
              <Icon name="lock" size={18} />
              <input
                id="password"
                type={showPassword ? "text" : "password"}
                autoComplete={isSignUp ? "new-password" : "current-password"}
                value={password}
                onChange={(e) => setPassword(e.target.value)}
                placeholder={isSignUp ? t("signup.passwordPh") : t("login.passwordPh")}
              />
              <button
                type="button"
                className="lp-eye"
                onClick={() => setShowPassword((v) => !v)}
                aria-label={showPassword ? t("login.hide") : t("login.show")}
              >
                <Icon name={showPassword ? "eyeOff" : "eye"} size={20} />
              </button>
            </div>
          </div>

          <label className="lp-check">
            <input
              type="checkbox"
              checked={remember}
              onChange={(e) => setRemember(e.target.checked)}
            />
            <span>{t("login.remember")}</span>
          </label>

          {error && <p className="form-error" role="alert">{error}</p>}
          {notice && <p className="form-notice" role="status">{notice}</p>}

          <button type="submit" className="lp-submit" disabled={busy}>
            {busy && <span className="spinner" aria-hidden="true" />}
            {busy
              ? (isSignUp ? t("signup.submitting") : t("login.submitting"))
              : (isSignUp ? t("signup.submit") : t("login.submit"))}
            {!busy && (
              <svg className="icon" width="20" height="20" viewBox="0 0 24 24" aria-hidden="true">
                <path d="M5 12h14M13 6l6 6-6 6" />
              </svg>
            )}
          </button>

          <p className="lp-foot">
            {isSignUp ? t("signup.haveAccount") : t("login.newHere")}{" "}
            <button
              type="button"
              className="lp-link"
              onClick={() => {
                setMode(isSignUp ? "signin" : "signup");
                setError("");
                setNotice("");
              }}
            >
              {isSignUp ? t("signup.toSignIn") : t("login.createAccount")}
            </button>
          </p>
        </form>

        <ul className="lp-pills">
          <li><Icon name="clock" size={18} />{t("login.pill1")}</li>
          <li><Icon name="chart" size={18} />{t("login.pill2")}</li>
          <li><Icon name="bell" size={18} />{t("login.pill3")}</li>
        </ul>
      </main>
    </div>
  );
}
