PRAGMA journal_mode       = WAL;
PRAGMA synchronous        = NORMAL;
PRAGMA busy_timeout       = 5000;
PRAGMA cache_size         = -64000;
PRAGMA temp_store         = MEMORY;
PRAGMA mmap_size          = 268435456;
PRAGMA foreign_keys       = ON;
PRAGMA journal_size_limit = 67108864;

CREATE TABLE IF NOT EXISTS audit_log (
    id              INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    create_user_id  INTEGER,
    record_id       INTEGER NOT NULL,
    table_name      TEXT    NOT NULL,
    changes         TEXT    NOT NULL  DEFAULT '{}',
    priority        INTEGER NOT NULL  DEFAULT 1  CHECK (priority IN (0, 1, 2)),
    event_timestamp INTEGER NOT NULL,
    created_at      INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS user_audit_log (
    id              INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id         INTEGER NOT NULL,
    record_id       INTEGER NOT NULL,
    table_name      TEXT    NOT NULL,
    changes         TEXT    NOT NULL  DEFAULT '{}',
    priority        INTEGER NOT NULL  DEFAULT 1  CHECK (priority IN (0, 1, 2)),
    event_timestamp INTEGER NOT NULL,
    created_at      INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS audit_compaction_state (
    table_name           TEXT    NOT NULL PRIMARY KEY,
    compacted_through_id INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS user_action_log (
    id         INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id    INTEGER NOT NULL,
    record_id  INTEGER NOT NULL,
    table_name TEXT    NOT NULL,
    action     TEXT    NOT NULL  CHECK (action IN ('create', 'read', 'update', 'delete')),
    old_data   TEXT    NOT NULL  DEFAULT '{}',
    new_data   TEXT    NOT NULL  DEFAULT '{}',
    ip_address TEXT    NOT NULL  DEFAULT '',
    msg_id     TEXT    NOT NULL  DEFAULT '',
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

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

CREATE INDEX IF NOT EXISTS idx_audit_log_record   ON audit_log (record_id, table_name);
CREATE INDEX IF NOT EXISTS idx_audit_log_table_ts ON audit_log (table_name, event_timestamp);

CREATE INDEX IF NOT EXISTS idx_user_audit_log_user_ts ON user_audit_log (user_id, event_timestamp);
CREATE INDEX IF NOT EXISTS idx_user_audit_log_record   ON user_audit_log (record_id, table_name);

CREATE INDEX IF NOT EXISTS idx_notification_delivery_inbox_status
    ON notification_delivery_inbox (status, delivery_id);

CREATE UNIQUE INDEX IF NOT EXISTS idx_user_action_log_msg_id
    ON user_action_log (msg_id) WHERE msg_id <> '';
