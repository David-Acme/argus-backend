-- ─────────────────────────────────────────────────────────────────────────────
-- Argus sync  ·  Sync schema (identity.db until Phase 3c-2 splits the file)
-- The four tables this owner writes: the two audit logs, the user action
-- journal and the notification delivery inbox, plus the five indexes they
-- carry — moved verbatim out of packages/identity's schema (source of truth).
-- The audit and journal rows reference identity's user(id); the sync service
-- applies only its own tables to the file it opens at boot.
-- Structure: pragmas → table creation → indexes (grouped by table).
-- ─────────────────────────────────────────────────────────────────────────────

PRAGMA journal_mode       = WAL;
PRAGMA synchronous        = NORMAL;
PRAGMA busy_timeout       = 5000;
PRAGMA cache_size         = -64000;
PRAGMA temp_store         = MEMORY;
PRAGMA mmap_size          = 268435456;
PRAGMA foreign_keys       = ON;
PRAGMA journal_size_limit = 67108864;

-- ── Tables · Audit ───────────────────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS audit_log (
    id              INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    create_user_id  INTEGER           REFERENCES user(id) ON DELETE SET NULL,
    record_id       INTEGER NOT NULL,
    table_name      TEXT    NOT NULL,
    changes         TEXT    NOT NULL  DEFAULT '{}',   -- JSON diff (JsonDiff::toJson)
    priority        INTEGER NOT NULL  DEFAULT 1  CHECK (priority IN (0, 1, 2)),
    event_timestamp INTEGER NOT NULL,
    created_at      INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS user_audit_log (
    id              INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id         INTEGER NOT NULL  REFERENCES user(id) ON DELETE CASCADE,
    record_id       INTEGER NOT NULL,
    table_name      TEXT    NOT NULL,
    changes         TEXT    NOT NULL  DEFAULT '{}',   -- JSON diff (JsonDiff::toJson)
    priority        INTEGER NOT NULL  DEFAULT 1  CHECK (priority IN (0, 1, 2)),
    event_timestamp INTEGER NOT NULL,
    created_at      INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS user_action_log (
    id         INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id    INTEGER NOT NULL  REFERENCES user(id) ON DELETE CASCADE,
    record_id  INTEGER NOT NULL,
    table_name TEXT    NOT NULL,
    action     TEXT    NOT NULL  CHECK (action IN ('create', 'read', 'update', 'delete')),
    old_data   TEXT    NOT NULL  DEFAULT '{}',
    new_data   TEXT    NOT NULL  DEFAULT '{}',
    ip_address TEXT    NOT NULL  DEFAULT '',
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

-- ── Tables · Delivery inbox ──────────────────────────────────────────────────

-- Durable receipts for argus.notification.v1.delivery, written only by the
-- delivery consumer in this same database. The insert wins the dispatch lease;
-- a 'dispatched' row drops redeliveries, a 'received' row replays them.
-- fingerprint is the canonical payload hash: the same id plus the same
-- fingerprint is a replay, the same id plus a different fingerprint is a
-- conflict that is never dispatched. 'dead_lettered' rows are poison the
-- broker must not resend.
CREATE TABLE IF NOT EXISTS notification_delivery_inbox (
    delivery_id     INTEGER NOT NULL  PRIMARY KEY,
    notification_id INTEGER NOT NULL  DEFAULT 0,
    user_id         INTEGER NOT NULL  DEFAULT 0,
    fingerprint     TEXT    NOT NULL  DEFAULT '',
    attempts        INTEGER NOT NULL  DEFAULT 0,
    status          TEXT    NOT NULL  DEFAULT 'received'
                    CHECK (status IN ('received', 'dispatched', 'conflict',
                                      'dead_lettered')),
    created_at      INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at      INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

-- ── Indexes ──────────────────────────────────────────────────────────────────

-- audit_log
CREATE INDEX IF NOT EXISTS idx_audit_log_record   ON audit_log (record_id, table_name);
CREATE INDEX IF NOT EXISTS idx_audit_log_table_ts ON audit_log (table_name, event_timestamp);

-- user_audit_log
CREATE INDEX IF NOT EXISTS idx_user_audit_log_user_ts ON user_audit_log (user_id, event_timestamp);
CREATE INDEX IF NOT EXISTS idx_user_audit_log_record   ON user_audit_log (record_id, table_name);

-- notification_delivery_inbox
CREATE INDEX IF NOT EXISTS idx_notification_delivery_inbox_status
    ON notification_delivery_inbox (status, delivery_id);
