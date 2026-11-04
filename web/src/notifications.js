import { useEffect, useRef } from "react";

// Browser notifications for new alerts.
// Android Chrome only shows notifications through a service worker, so we
// register /sw.js (in the public folder) and send them through it.
// These work while SmartPig is open or running in the background. Alerts when
// the app is fully closed would need Firebase Cloud Messaging and a server.

export function notificationSupport() {
  if (typeof window === "undefined" || !("Notification" in window)) return "unsupported";
  return Notification.permission; // "default" | "granted" | "denied"
}

export function registerServiceWorker() {
  if (typeof navigator === "undefined" || !("serviceWorker" in navigator)) return;
  window.addEventListener("load", () => {
    navigator.serviceWorker.register("/sw.js").catch(() => {
      // Not fatal: notifications fall back to the plain Notification API
    });
  });
}

export async function requestNotificationPermission() {
  if (notificationSupport() === "unsupported") return "unsupported";
  try {
    return await Notification.requestPermission();
  } catch {
    return Notification.permission;
  }
}

export async function showNotification(title, body, tag) {
  if (notificationSupport() !== "granted") return;
  const options = {
    body,
    tag,
    renotify: true,
    icon: "/icons/icon-192.png",
    badge: "/icons/badge-96.png",
  };
  try {
    if ("serviceWorker" in navigator) {
      const reg = await navigator.serviceWorker.getRegistration();
      if (reg) {
        await reg.showNotification(title, options);
        return;
      }
    }
    new Notification(title, options);
  } catch {
    // Some browsers refuse silently; the alert is still shown on screen
  }
}

// Sends a notification when an alert appears that wasn't there before.
// Alerts that are already showing when the page loads are not re-sent.
export function useAlertNotifications(alerts, ready, enabled, toText) {
  const seen = useRef(null);
  const ids = alerts.map((a) => a.id).join("|");

  useEffect(() => {
    if (!ready) return;
    const current = new Set(alerts.map((a) => a.id));
    if (seen.current === null) {
      seen.current = current;
      return;
    }
    if (enabled) {
      for (const a of alerts) {
        if (!seen.current.has(a.id)) {
          const { title, text } = toText(a);
          showNotification(`SmartPig: ${title}`, text, `smartpig-${a.id}`);
        }
      }
    }
    seen.current = current;
    // Only re-run when the set of alerts changes, not every second
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ids, ready, enabled]);
}
