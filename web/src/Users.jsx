import { useEffect, useState } from "react";
import { subscribeToUsers, setUserRole } from "./dataService";
import { formatAge, formatManila } from "./deviceData";
import { useSettings } from "./settings";
import Icon from "./icons";

// Everyone who signs up is a Farmer with full access. The only control left is
// "Remove access" (for a stranger) and "Restore access".
function UserRow({ u, isMe, now, saving, onRole }) {
  const { t, lang } = useSettings();
  const removed = u.role === "disabled";
  const joined = u.createdAt ? formatManila(u.createdAt, lang).split(",").slice(0, 2).join(",") : null;
  const [confirming, setConfirming] = useState(false);

  return (
    <div className={`user-row ${removed ? "user-removed" : ""}`}>
      <span className="avatar" aria-hidden="true">
        {(u.name || u.email || "?").charAt(0).toUpperCase()}
      </span>
      <div className="user-info">
        <strong>
          {u.name || t("users.noName")}
          {isMe && <span className="you"> {t("users.you")}</span>}
        </strong>
        <span className="subtle">{u.email || u.uid}</span>
        <span className="user-meta">
          <span className={`tag ${removed ? "tag-red" : "tag-green"}`}>
            {removed ? t("role.disabled") : t("role.user")}
          </span>
          {u.lastDevice && <span><Icon name="phone" size={14} />{u.lastDevice}</span>}
          <span>
            <span className={`dot ${u.lastSeenAt && now - u.lastSeenAt < 600000 ? "live" : ""}`} />
            {u.lastSeenAt ? t("users.seen", { age: formatAge(Math.max(0, now - u.lastSeenAt), lang) }) : "—"}
          </span>
          {typeof u.signInCount === "number" && <span>{t("users.signIns", { n: u.signInCount })}</span>}
          {joined && <span>{t("users.joined", { date: joined })}</span>}
        </span>
      </div>

      {!isMe && (
        <div className="user-actions">
          {removed ? (
            <button type="button" className="btn btn-ghost btn-sm" disabled={saving} onClick={() => onRole("user")}>
              {t("users.restore")}
            </button>
          ) : confirming ? (
            <>
              <button type="button" className="btn btn-ghost btn-sm" onClick={() => setConfirming(false)}>
                {t("feed.cancel")}
              </button>
              <button
                type="button"
                className="btn btn-danger btn-sm"
                disabled={saving}
                onClick={() => {
                  setConfirming(false);
                  onRole("disabled");
                }}
              >
                {t("users.removeYes")}
              </button>
            </>
          ) : (
            <button type="button" className="btn btn-ghost btn-sm btn-danger-ghost" onClick={() => setConfirming(true)}>
              {t("users.remove")}
            </button>
          )}
        </div>
      )}
    </div>
  );
}

export default function Users({ currentUid }) {
  const { t } = useSettings();
  const [users, setUsers] = useState([]);
  const [loaded, setLoaded] = useState(false);
  const [error, setError] = useState("");
  const [message, setMessage] = useState("");
  const [savingUid, setSavingUid] = useState(null);
  const [now, setNow] = useState(Date.now());

  useEffect(() => {
    return subscribeToUsers(
      (rows) => {
        setUsers(rows);
        setLoaded(true);
      },
      (err) => {
        setError(err.message);
        setLoaded(true);
      }
    );
  }, []);

  // Refresh the "x min ago" texts
  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), 30000);
    return () => clearInterval(id);
  }, []);

  async function changeRole(uid, role) {
    setSavingUid(uid);
    setMessage("");
    try {
      await setUserRole(uid, role);
    } catch (err) {
      setMessage(t("users.roleError", { error: err.message }));
    } finally {
      setSavingUid(null);
    }
  }

  const active = users.filter((u) => u.role !== "disabled");
  const removed = users.filter((u) => u.role === "disabled");
  const activeToday = users.filter((u) => u.lastSeenAt && now - u.lastSeenAt < 86400000).length;

  const row = (u) => (
    <UserRow
      key={u.uid}
      u={u}
      isMe={u.uid === currentUid}
      now={now}
      saving={savingUid === u.uid}
      onRole={(r) => changeRole(u.uid, r)}
    />
  );

  return (
    <div className="dashboard page-enter">
      <div className="grid-3 users-stats">
        <div className="card stat">
          <span className="card-label"><Icon name="users" size={16} />{t("users.farmers")}</span>
          <span className="value">{loaded ? active.length : "—"}</span>
        </div>
        <div className="card stat">
          <span className="card-label"><Icon name="user" size={16} />{t("users.today")}</span>
          <span className="value">{loaded ? activeToday : "—"}</span>
        </div>
        <div className="card stat">
          <span className="card-label"><Icon name="lock" size={16} />{t("users.removedN")}</span>
          <span className="value">{loaded ? removed.length : "—"}</span>
        </div>
      </div>

      {message && <p className="form-error" role="alert">{message}</p>}
      {error && <p className="form-error" role="alert">{t("users.loadError", { error })}</p>}
      {!loaded && <span className="skeleton" style={{ height: 160 }} />}

      {loaded && !error && (
        <section className="card user-list">
          <span className="card-label"><Icon name="users" size={16} />{t("users.all")}</span>
          {active.map(row)}
          {removed.map(row)}
          {users.length === 0 && <p className="subtle">{t("users.none")}</p>}
        </section>
      )}

      <p className="subtle small-note">{t("users.note")}</p>
    </div>
  );
}
