PRAGMA journal_mode       = WAL;
PRAGMA synchronous        = NORMAL;
PRAGMA busy_timeout       = 5000;
PRAGMA cache_size         = -64000;
PRAGMA temp_store         = MEMORY;
PRAGMA mmap_size          = 268435456;
PRAGMA foreign_keys       = ON;
PRAGMA journal_size_limit = 67108864;

CREATE TABLE IF NOT EXISTS camera (
    id             INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    name           TEXT    NOT NULL,
    manufacturer   TEXT    NOT NULL  DEFAULT '',
    model          TEXT    NOT NULL  DEFAULT '',
    ip             TEXT    NOT NULL,
    port           INTEGER NOT NULL  DEFAULT 554,
    username       TEXT    NOT NULL  DEFAULT 'admin',
    password       TEXT    NOT NULL  DEFAULT '',
    cloud_username TEXT    NOT NULL  DEFAULT '',
    cloud_password TEXT    NOT NULL  DEFAULT '',
    driver         TEXT    NOT NULL  DEFAULT 'tapo'
                                     CHECK (driver IN ('tapo', 'onvif', 'rtsp')),
    icon           TEXT    NOT NULL  DEFAULT 'video',
    record_mode    TEXT    NOT NULL  DEFAULT 'events'
                                     CHECK (record_mode IN ('events', 'continuous')),
    retention_days INTEGER,
    capabilities   TEXT    NOT NULL  DEFAULT '[]',
    config         TEXT    NOT NULL  DEFAULT '{}',
    is_enabled     INTEGER NOT NULL  DEFAULT 1  CHECK (is_enabled IN (0, 1)),
    is_online      INTEGER NOT NULL  DEFAULT 0,
    created_at     INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at     INTEGER,
    deleted_at     INTEGER
);

CREATE TABLE IF NOT EXISTS camera_stream (
    id         INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    camera_id  INTEGER NOT NULL  REFERENCES camera(id) ON DELETE CASCADE,
    label      TEXT    NOT NULL  DEFAULT '',
    url        TEXT    NOT NULL,
    resolution TEXT    NOT NULL  DEFAULT '',
    fps        INTEGER NOT NULL  DEFAULT 0,
    codec      TEXT    NOT NULL  DEFAULT '',
    is_primary INTEGER NOT NULL  DEFAULT 1,
    is_enabled INTEGER NOT NULL  DEFAULT 1,
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at INTEGER,
    deleted_at INTEGER
);

CREATE TABLE IF NOT EXISTS zone (
    id         INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    camera_id  INTEGER NOT NULL  REFERENCES camera(id) ON DELETE CASCADE,
    name       TEXT    NOT NULL,
    points     TEXT    NOT NULL,
    zone_type  TEXT    NOT NULL  DEFAULT 'monitor'
                                     CHECK (zone_type IN ('monitor', 'alert', 'exclude')),
    color      TEXT    NOT NULL  DEFAULT '#FF0000',
    is_enabled INTEGER NOT NULL  DEFAULT 1,
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at INTEGER,
    deleted_at INTEGER
);

CREATE INDEX IF NOT EXISTS idx_camera_created_at  ON camera (created_at);
CREATE INDEX IF NOT EXISTS idx_camera_deleted_at  ON camera (deleted_at);

CREATE INDEX IF NOT EXISTS idx_camera_stream_camera   ON camera_stream (camera_id);
CREATE INDEX IF NOT EXISTS idx_camera_stream_created  ON camera_stream (created_at);
CREATE INDEX IF NOT EXISTS idx_camera_stream_deleted  ON camera_stream (deleted_at);

CREATE INDEX IF NOT EXISTS idx_zone_camera_id   ON zone (camera_id);
CREATE INDEX IF NOT EXISTS idx_zone_created_at  ON zone (created_at);
CREATE INDEX IF NOT EXISTS idx_zone_deleted_at  ON zone (deleted_at);

CREATE TABLE IF NOT EXISTS action_command (
    command_id TEXT    NOT NULL  PRIMARY KEY,
    kind       TEXT    NOT NULL  DEFAULT '',
    camera_id  INTEGER NOT NULL  DEFAULT 0,
    status     TEXT    NOT NULL  DEFAULT 'executing',
    detail     TEXT    NOT NULL  DEFAULT '',
    response   TEXT    NOT NULL  DEFAULT '',
    fingerprint TEXT   NOT NULL  DEFAULT '',
    attempts   INTEGER NOT NULL  DEFAULT 0,
    generation INTEGER NOT NULL  DEFAULT 0,
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS siren_lease (
    camera_id  INTEGER NOT NULL  PRIMARY KEY,
    command_id TEXT    NOT NULL  DEFAULT '',
    expires_at INTEGER NOT NULL,
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_action_command_created
    ON action_command (created_at DESC);
CREATE INDEX IF NOT EXISTS idx_siren_lease_expires
    ON siren_lease (expires_at);

CREATE TABLE IF NOT EXISTS camera_evidence (
    id           INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    camera_id    INTEGER NOT NULL  DEFAULT 0,
    object_key   TEXT    NOT NULL  DEFAULT '',
    content_type TEXT    NOT NULL  DEFAULT '',
    created_at   INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    expires_at   INTEGER NOT NULL  DEFAULT 0,
    deleted_at   INTEGER NOT NULL  DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_camera_evidence_expires
    ON camera_evidence (expires_at) WHERE deleted_at = 0;

CREATE TABLE IF NOT EXISTS object_event_outbox (
    event_id   TEXT    NOT NULL  PRIMARY KEY,
    payload    TEXT    NOT NULL,
    status     TEXT    NOT NULL  DEFAULT 'pending'
                       CHECK (status IN ('pending', 'sent', 'overflow_dropped')),
    attempts   INTEGER NOT NULL  DEFAULT 0,
    created_at INTEGER NOT NULL  DEFAULT 0,
    sent_at    INTEGER NOT NULL  DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_object_event_outbox_status
    ON object_event_outbox (status, created_at);

CREATE TABLE IF NOT EXISTS camera_event_cooldown (
    camera_id    INTEGER NOT NULL,
    class        TEXT    NOT NULL,
    last_emit_ms INTEGER NOT NULL  DEFAULT 0,
    PRIMARY KEY (camera_id, class)
);

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
