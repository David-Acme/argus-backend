PRAGMA journal_mode       = WAL;
PRAGMA synchronous        = NORMAL;
PRAGMA busy_timeout       = 5000;
PRAGMA cache_size         = -64000;
PRAGMA temp_store         = MEMORY;
PRAGMA mmap_size          = 268435456;
PRAGMA foreign_keys       = OFF;
PRAGMA journal_size_limit = 67108864;

CREATE TABLE IF NOT EXISTS notification (
    id         INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id    INTEGER NOT NULL  REFERENCES user(id) ON DELETE CASCADE,
    type       TEXT    NOT NULL  DEFAULT 'system',
    title      TEXT    NOT NULL  DEFAULT '',
    body       TEXT    NOT NULL  DEFAULT '',
    data       TEXT    NOT NULL  DEFAULT '{}',
    is_read    INTEGER NOT NULL  DEFAULT 0  CHECK (is_read IN (0, 1)),
    read_at    INTEGER,
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_notification_user_created ON notification (user_id, created_at);

CREATE TABLE IF NOT EXISTS notification_token (
    id          INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id     INTEGER NOT NULL  REFERENCES user(id) ON DELETE CASCADE,
    device_hash TEXT    NOT NULL  DEFAULT '',
    token       TEXT    NOT NULL,
    platform    TEXT    NOT NULL  DEFAULT '',
    lang        TEXT    NOT NULL  DEFAULT '',
    is_active   INTEGER NOT NULL  DEFAULT 1  CHECK (is_active IN (0, 1)),
    created_at  INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at  INTEGER
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_notification_token_uniq
    ON notification_token (user_id, device_hash);

CREATE TABLE IF NOT EXISTS notification_command (
    command_id     TEXT    NOT NULL  PRIMARY KEY,
    expected_count INTEGER NOT NULL  DEFAULT 0,
    fingerprint    TEXT    NOT NULL  DEFAULT '',
    created_at     INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS notification_delivery (
    id              INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    notification_id INTEGER NOT NULL  REFERENCES notification(id) ON DELETE CASCADE,
    user_id         INTEGER NOT NULL  DEFAULT 0,
    status          TEXT    NOT NULL  DEFAULT 'pending'
                            CHECK (status IN ('pending', 'sent')),
    attempts        INTEGER NOT NULL  DEFAULT 0,
    created_at      INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    sent_at         INTEGER NOT NULL  DEFAULT 0,
    acked_at        INTEGER NOT NULL  DEFAULT 0,
    created_ms      INTEGER NOT NULL  DEFAULT 0,
    sent_ms         INTEGER NOT NULL  DEFAULT 0,
    acked_ms        INTEGER NOT NULL  DEFAULT 0
);

CREATE TABLE IF NOT EXISTS notification_selftest (
    id      INTEGER NOT NULL  PRIMARY KEY CHECK (id = 1),
    last_at INTEGER NOT NULL  DEFAULT 0,
    last_ok INTEGER NOT NULL  DEFAULT 0,
    last_ms INTEGER NOT NULL  DEFAULT 0
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_notification_delivery_notification
    ON notification_delivery (notification_id);
CREATE INDEX IF NOT EXISTS idx_notification_delivery_status
    ON notification_delivery (status, id);
CREATE INDEX IF NOT EXISTS idx_notification_token_user ON notification_token (user_id);

CREATE TABLE IF NOT EXISTS change_outbox (
    event_id    TEXT    NOT NULL  PRIMARY KEY,
    fingerprint TEXT    NOT NULL  DEFAULT '',
    payload     TEXT    NOT NULL,
    status      TEXT    NOT NULL  DEFAULT 'pending'
                        CHECK (status IN ('pending', 'sent')),
    attempts    INTEGER NOT NULL  DEFAULT 0,
    created_at  INTEGER NOT NULL  DEFAULT 0,
    sent_at     INTEGER NOT NULL  DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_change_outbox_status
    ON change_outbox (status, created_at);
