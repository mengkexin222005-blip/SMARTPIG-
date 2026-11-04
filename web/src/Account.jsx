import { useState } from "react";
import Icon from "./icons";
import { useSettings } from "./settings";
import { LANGS } from "./i18n";
import { formatManila } from "./deviceData";
import { changePassword, authErrorKey, signOutUser } from "./authService";
import {
  notificationSupport,
  requestNotificationPermission,
  showNotification,
} from "./notifications";

const ROLE_TAG = { admin: "tag-green", user: "tag-green", pending: "tag-green", disabled: "tag-red" };

function Segmented({ label, value, options, onChange }) {
  return (
    <div className="segmented" role="group" aria-label={label}>
      {options.map((o) => (
        <button
          key={o.id}
          type="button"
          className={value === o.id ? "on" : ""}
          aria-pressed={value === o.id}
          onClick={() => onChange(o.id)}
        >
          {o.icon && <Icon name={o.icon} size={16} />}
          {o.label}
        </button>
      ))}
    </div>
  );
}

function PasswordForm({ t }) {
  const [current, setCurrent] = useState("");
  const [next, setNext] = useState("");
  const [confirm, setConfirm] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [done, setDone] = useState("");

  async function handleSubmit(e) {
    e.preventDefault();
    setError("");
    setDone("");
    if (!current) return setError(t("acc.needCurrent"));
    if (next.length < 6) return setError(t("acc.short"));
    if (next !== confirm) return setError(t("acc.mismatch"));
    setBusy(true);
    try {
      await changePassword(current, next);
      setDone(t("acc.saved"));
      setCurrent("");
      setNext("");
      setConfirm("");
    } catch (err) {
      setError(t(authErrorKey(err)));
    } finally {
      setBusy(false);
    }
  }

  return (
    <form className="card list-card" onSubmit={handleSubmit} noValidate>
      <span className="card-label"><Icon name="lock" size={16} />{t("acc.password")}</span>
      <div className="form-grid">
        <div className="field span-2">
          <label htmlFor="pw-current">{t("acc.current")}</label>
          <input
            id="pw-current"
            type="password"
            autoComplete="current-password"
            value={current}
            onChange={(e) => setCurrent(e.target.value)}
          />
        </div>
        <div className="field">
          <label htmlFor="pw-new">{t("acc.new")}</label>
          <input
            id="pw-new"
            type="password"
            autoComplete="new-password"
            value={next}
            onChange={(e) => setNext(e.target.value)}
          />
        </div>
        <div className="field">
          <label htmlFor="pw-confirm">{t("acc.confirm")}</label>
          <input
            id="pw-confirm"
            type="password"
            autoComplete="new-password"
            value={confirm}
            onChange={(e) => setConfirm(e.target.value)}
          />
        </div>
      </div>
      {error && <p className="form-error" role="alert">{error}</p>}
      {done && <p className="form-notice" role="status">{done}</p>}
      <div className="form-actions">
        <button type="submit" className="btn btn-primary" disabled={busy}>
          {busy && <span className="spinner" aria-hidden="true" />}
          {busy ? t("acc.saving") : t("acc.save")}
        </button>
      </div>
    </form>
  );
}

// Every signed-in person's own page: profile, preferences, password
export default function Account({ user, profile }) {
  const { t, lang, setLang, theme, setTheme, notify, setNotify } = useSettings();
  const [permission, setPermission] = useState(notificationSupport);

  const name = profile.name || user.email;

  async function toggleNotify() {
    if (notify) {
      setNotify(false);
      return;
    }
    const result = await requestNotificationPermission();
    setPermission(result);
    if (result === "granted") setNotify(true);
  }

  return (
    <div className="dashboard page-enter">
      <section className="card profile-card">
        <span className="avatar avatar-lg" aria-hidden="true">
          {name.charAt(0).toUpperCase()}
        </span>
        <div className="user-info">
          <strong className="profile-name">{name}</strong>
          <span className="subtle">{user.email}</span>
        </div>
        <div className="profile-tags">
          <span className={`tag ${ROLE_TAG[profile.role || "user"] || "tag-grey"} round`}>
            {t(`role.${profile.role || "user"}`)}
          </span>
          {profile.createdAt && (
            <span className="subtle">
              {t("acc.since", { date: formatManila(profile.createdAt, lang).split(",").slice(0, 2).join(",").trim() })}
            </span>
          )}
        </div>
      </section>

      <div className="grid-2">
        <section className="card list-card">
          <span className="card-label"><Icon name="globe" size={16} />{t("acc.prefs")}</span>

          <div className="pref">
            <span className="pref-label">{t("acc.language")}</span>
            <Segmented
              label={t("acc.language")}
              value={lang}
              onChange={setLang}
              options={LANGS.map((l) => ({ id: l.id, label: l.label }))}
            />
          </div>

          <div className="pref">
            <span className="pref-label">{t("acc.theme")}</span>
            <Segmented
              label={t("acc.theme")}
              value={theme}
              onChange={setTheme}
              options={[
                { id: "system", label: t("acc.themeSystem") },
                { id: "light", label: t("acc.themeLight"), icon: "sun" },
                { id: "dark", label: t("acc.themeDark"), icon: "moon" },
              ]}
            />
          </div>

          <div className="divider" />

          <div className="pref pref-row">
            <div className="pref-text">
              <span className="pref-label" id="notify-label">{t("acc.notify")}</span>
              <span className="subtle">{t("acc.notifyText")}</span>
            </div>
            <button
              type="button"
              role="switch"
              aria-checked={notify && permission === "granted"}
              aria-labelledby="notify-label"
              className="switch"
              disabled={permission === "unsupported" || permission === "denied"}
              onClick={toggleNotify}
            >
              <span className="switch-knob" />
            </button>
          </div>
          {permission === "denied" && <p className="form-error">{t("acc.notifyBlocked")}</p>}
          {permission === "unsupported" && <p className="subtle">{t("acc.notifyUnsupported")}</p>}
          {notify && permission === "granted" && (
            <div className="pref-row">
              <span className="subtle">{t("acc.notifyOpenOnly")}</span>
              <button
                type="button"
                className="btn btn-ghost btn-sm"
                onClick={() =>
                  showNotification(t("acc.notifyTestTitle"), t("acc.notifyTestBody"), "smartpig-test")
                }
              >
                {t("acc.notifyTest")}
              </button>
            </div>
          )}
        </section>

        <PasswordForm t={t} />
      </div>

      <section className="card list-card">
        <span className="card-label"><Icon name="phone" size={16} />{t("acc.install")}</span>
        <p className="subtle">{t("acc.installText")}</p>
      </section>

      <button type="button" className="btn btn-ghost signout-wide" onClick={signOutUser}>
        <Icon name="logout" size={18} />
        {t("app.signOut")}
      </button>
    </div>
  );
}
