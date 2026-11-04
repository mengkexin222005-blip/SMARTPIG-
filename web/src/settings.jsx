import { createContext, useCallback, useContext, useEffect, useMemo, useState } from "react";
import { translate } from "./i18n";

// Per-device preferences: language, light/dark, notifications.
// Saved in this browser only (localStorage), so each phone keeps its own.

const KEY = "smartpig.settings.v1";
const DEFAULTS = { lang: "en", theme: "system", notify: false };

function load() {
  try {
    const raw = window.localStorage.getItem(KEY);
    return raw ? { ...DEFAULTS, ...JSON.parse(raw) } : DEFAULTS;
  } catch {
    return DEFAULTS;
  }
}

function save(value) {
  try {
    window.localStorage.setItem(KEY, JSON.stringify(value));
  } catch {
    // Private mode or storage blocked: settings just won't be remembered
  }
}

function systemPrefersDark() {
  return (
    typeof window !== "undefined" &&
    window.matchMedia &&
    window.matchMedia("(prefers-color-scheme: dark)").matches
  );
}

const SettingsContext = createContext(null);

export function SettingsProvider({ children }) {
  const [settings, setSettings] = useState(load);
  const [systemDark, setSystemDark] = useState(systemPrefersDark);

  // Follow the phone/computer dark-mode setting when theme is "system"
  useEffect(() => {
    if (!window.matchMedia) return undefined;
    const mq = window.matchMedia("(prefers-color-scheme: dark)");
    const onChange = (e) => setSystemDark(e.matches);
    mq.addEventListener("change", onChange);
    return () => mq.removeEventListener("change", onChange);
  }, []);

  const resolvedTheme =
    settings.theme === "system" ? (systemDark ? "dark" : "light") : settings.theme;

  useEffect(() => {
    const root = document.documentElement;
    root.dataset.theme = resolvedTheme;
    root.lang = settings.lang === "tl" ? "tl" : "en";
    const meta = document.querySelector('meta[name="theme-color"]');
    if (meta) meta.setAttribute("content", resolvedTheme === "dark" ? "#13261C" : "#EEF8F2");
  }, [resolvedTheme, settings.lang]);

  const update = useCallback((patch) => {
    setSettings((prev) => {
      const next = { ...prev, ...patch };
      save(next);
      return next;
    });
  }, []);

  const value = useMemo(
    () => ({
      ...settings,
      resolvedTheme,
      setLang: (lang) => update({ lang }),
      setTheme: (theme) => update({ theme }),
      setNotify: (notify) => update({ notify }),
      t: (key, vars) => translate(settings.lang, key, vars),
    }),
    [settings, resolvedTheme, update]
  );

  return <SettingsContext.Provider value={value}>{children}</SettingsContext.Provider>;
}

export function useSettings() {
  const ctx = useContext(SettingsContext);
  if (!ctx) throw new Error("useSettings must be used inside <SettingsProvider>");
  return ctx;
}
