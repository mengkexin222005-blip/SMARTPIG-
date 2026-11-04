import { useEffect, useState } from "react";
import { subscribeToAuth, signOutUser } from "./authService";
import { subscribeToProfile, recordVisit, canControl } from "./dataService";
import { useDeviceData, formatAge, alertText } from "./deviceData";
import { useSettings } from "./settings";
import { useAlertNotifications } from "./notifications";
import Login, { LangSwitch } from "./Login";
import Overview from "./Overview";
import History from "./History";
import System from "./System";
import Account from "./Account";
import Feeding from "./Feeding";
import { useFeedingConfig, climateLimits } from "./feedingData";
import { LogoMark, Wordmark } from "./Logo";
import Icon from "./icons";
import "./App.css";

// Visitors (not signed in) can look at the readings. Everyone who signs up is a
// Farmer with full access. An account set to "No access" can only look.
const PAGES = {
  overview: { icon: "grid" },
  feeding: { icon: "bowl" },
  history: { icon: "chart" },
  system: { icon: "cpu" },
  account: { icon: "user" },
};
// System (device health + thesis test results) needs a Farmer account
const PUBLIC_PAGES = ["overview", "feeding", "history"];
const VISITOR_PAGES = ["overview"];                        // not signed in: Overview only
const ACCOUNT_PAGES = [...PUBLIC_PAGES, "account"];       // signed in, access removed
const FARMER_PAGES = [...PUBLIC_PAGES, "system", "account"];

// Shown to an account whose access was removed
function AccessNotice({ role, t }) {
  if (role !== "disabled") return null;
  return (
    <div className="access-notice access-removed" role="status">
      <Icon name="lock" size={20} />
      <div>
        <strong>{t("access.removedTitle")}</strong>
        <span>{t("access.removedText")}</span>
      </div>
    </div>
  );
}

function CenterMessage({ title, text, action, loading }) {
  return (
    <div className="center-screen">
      <section className="card center-card page-enter">
        <div className="center-brand">
          <LogoMark size={36} />
          <Wordmark size={20} />
        </div>
        <h1>
          {loading && <span className="spinner spinner-dark" aria-hidden="true" />}
          {title}
        </h1>
        {text && <p className="subtle">{text}</p>}
        {action}
      </section>
    </div>
  );
}

// user = null for a visitor (public view)
function Dashboard({ user, profile, onSignIn }) {
  const { t, lang, notify } = useSettings();
  const { settings: feedSettings } = useFeedingConfig();
  const device = useDeviceData(
    Number(feedSettings.heatAlertC) || undefined,
    Number(feedSettings.hopperLowGrams) || 2000,
    climateLimits(feedSettings)
  );
  const signedIn = Boolean(user);
  const role = signedIn && profile ? profile.role || "user" : null;
  const canFeed = canControl(role);
  const pages = !signedIn ? VISITOR_PAGES : canFeed ? FARMER_PAGES : ACCOUNT_PAGES;
  const [page, setPage] = useState(() => {
    const route = window.location.hash.slice(1);
    return Object.hasOwn(PAGES, route) ? route : "overview";
  });
  const current = pages.includes(page) ? page : "overview";

  useAlertNotifications(device.alerts, device.hasLoaded, notify, (a) => alertText(a, lang));

  useEffect(() => {
    function syncPageFromUrl() {
      const route = window.location.hash.slice(1);
      setPage(Object.hasOwn(PAGES, route) ? route : "overview");
    }
    window.addEventListener("popstate", syncPageFromUrl);
    return () => window.removeEventListener("popstate", syncPageFromUrl);
  }, []);

  // Show the alert count in the browser tab
  useEffect(() => {
    const n = device.alerts.length;
    document.title = n > 0 ? `(${n}) SmartPig` : "SmartPig";
  }, [device.alerts.length]);

  function go(id) {
    window.history.pushState(null, "", `#${id}`);
    setPage(id);
    window.scrollTo({ top: 0, behavior: "smooth" });
  }

  const navFor = (list) => list.map((id) => (
    <button
      key={id}
      type="button"
      className={`nav-btn ${current === id ? "active" : ""}`}
      aria-current={current === id ? "page" : undefined}
      onClick={() => go(id)}
    >
      <span className="nav-ico"><Icon name={PAGES[id].icon} /></span>
      <span className="nav-label">{t(`nav.${id}`)}</span>
      {id === "overview" && device.alerts.length > 0 && (
        <span className="nav-badge" aria-label={`${device.alerts.length} ${t("ov.alerts")}`}>
          {device.alerts.length}
        </span>
      )}
    </button>
  ));

  const displayName = signedIn ? (profile && profile.name) || user.email : "";
  const firstName = (displayName || "").trim().split(/[\s@]+/)[0];

  return (
    <div className="shell">
      {/* Laptop: left sidebar */}
      <aside className="sidebar">
        <div className="brand">
          <LogoMark size={44} />
          <div className="brand-text">
            <Wordmark size={22} />
            <span className="brand-sub">{t("side.tagline")}</span>
          </div>
        </div>

        <nav className="side-nav" aria-label="Main">
          <span className="side-section">{t("side.monitor")}</span>
          {navFor(pages.filter((id) => ["overview", "feeding", "history"].includes(id)))}
          {canFeed && (
            <>
              <span className="side-section">{t("side.manage")}</span>
              {navFor(pages.filter((id) => id === "system"))}
            </>
          )}
        </nav>

        <div className="side-foot">
          {signedIn ? (
            <button
              type="button"
              className={`side-account ${current === "account" ? "active" : ""}`}
              aria-current={current === "account" ? "page" : undefined}
              onClick={() => go("account")}
              title={t("nav.account")}
            >
              <span className="avatar" aria-hidden="true">
                {displayName.charAt(0).toUpperCase()}
              </span>
              <span className="user-info">
                <strong>{displayName}</strong>
                <span className="side-role">{role ? t(`role.${role}`) : user.email}</span>
              </span>
              <svg className="icon side-chev" width="18" height="18" viewBox="0 0 24 24" aria-hidden="true">
                <path d="m9 6 6 6-6 6" />
              </svg>
            </button>
          ) : null}
          <div className="side-lang">
            <Icon name="globe" size={16} />
            <LangSwitch />
          </div>
        </div>
      </aside>

      <div className="content">
        <header className="topbar">
          <div className="title-block">
            {/* Phone: logo on the left, text on the right */}
            <LogoMark size={56} className="mobile-only topbar-logo" />
            <div className="title-text">
              <span className="eyebrow mobile-only">
                SmartPig · {signedIn ? firstName : t("pub.badge")}
              </span>
              <h1>{t(`title.${current}`)}</h1>
              <span className="subtle">
                {device.lastUpdated !== null
                  ? t("time.updated", { age: formatAge(device.ageMs, lang) })
                  : t("time.never")}
              </span>
            </div>
          </div>
          <div className="topbar-right">
            <span className={`pill pill-${device.tone}`}>
              <span className={`dot ${device.tone === "ok" ? "live" : ""}`} />
              {t(device.statusKey)}
            </span>
            {signedIn ? (
              <>
                <button
                  type="button"
                  className={`icon-btn mobile-only ${current === "account" ? "icon-btn-on" : ""}`}
                  onClick={() => go("account")}
                  aria-label={t("nav.account")}
                  title={t("nav.account")}
                >
                  <Icon name="user" size={18} />
                </button>
                <button
                  type="button"
                  className="icon-btn"
                  onClick={signOutUser}
                  aria-label={t("app.signOut")}
                  title={t("app.signOut")}
                >
                  <Icon name="logout" size={18} />
                </button>
              </>
            ) : (
              <button type="button" className="btn btn-primary btn-sm topbar-signin" onClick={onSignIn}>
                <Icon name="lock" size={16} />
                {t("pub.signIn")}
              </button>
            )}
          </div>
        </header>

        {signedIn && profile && <AccessNotice role={role} t={t} />}

        <main key={current}>
          {current === "overview" && (
            <Overview
              device={device}
              name={firstName}
              onOpenFeeding={signedIn ? () => go("feeding") : undefined}
            />
          )}
          {current === "history" && (
            <div className="dashboard page-enter">
              <History />
            </div>
          )}
          {current === "system" && canFeed && <System device={device} />}
          {current === "feeding" && (
            <Feeding
              uid={signedIn ? user.uid : null}
              canEdit={canFeed}
              signedIn={signedIn}
              device={device}
              onSignIn={onSignIn}
            />
          )}
          {current === "account" && signedIn && <Account user={user} profile={profile || {}} />}
        </main>
      </div>

      {/* Phone: bottom tab bar */}
      {signedIn && (
        <nav className="bottom-nav" aria-label="Main">
          {navFor(pages.filter((id) => id !== "account"))}
        </nav>
      )}
    </div>
  );
}

export default function App() {
  const { t } = useSettings();
  // undefined = still checking, null = not signed in
  const [user, setUser] = useState(undefined);
  const [profile, setProfile] = useState(null);
  // Not signed in: the public Overview first; "Sign in" opens the login page.
  // #signin in the address keeps the login page open after a refresh.
  const [showLogin, setShowLogin] = useState(() => window.location.hash === "#signin");

  useEffect(() => {
    function syncLogin() {
      setShowLogin(window.location.hash === "#signin");
    }
    window.addEventListener("popstate", syncLogin);
    return () => window.removeEventListener("popstate", syncLogin);
  }, []);

  function openLogin() {
    window.history.pushState(null, "", "#signin");
    setShowLogin(true);
    window.scrollTo({ top: 0 });
  }
  function closeLogin() {
    window.history.pushState(null, "", window.location.pathname);
    setShowLogin(false);
  }

  useEffect(() => {
    return subscribeToAuth((u) => {
      setUser(u);
      setProfile(null);
      if (u) {
        setShowLogin(false);
        if (window.location.hash === "#signin") window.history.replaceState(null, "", window.location.pathname);
        // Save "last seen" for the Users page (creates the profile if it is missing)
        recordVisit(u).catch((e) => console.warn("Could not save visit:", e));
      }
    });
  }, []);

  useEffect(() => {
    if (!user) return undefined;
    return subscribeToProfile(
      user.uid,
      (p) => setProfile(p),
      () => setProfile(null)
    );
  }, [user]);

  if (user === undefined) {
    return <CenterMessage title={t("app.loading")} loading />;
  }

  if (!user) {
    return showLogin ? <Login onBack={closeLogin} /> : <Dashboard user={null} profile={null} onSignIn={openLogin} />;
  }

  return <Dashboard user={user} profile={profile} />;
}
