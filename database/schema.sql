-- ─────────────────────────────────────────────────────────────────────────────
-- Argus backend  ·  SQLite schema
-- Applied at startup via  DbService::runScriptFile("database/schema.sql")
-- ─────────────────────────────────────────────────────────────────────────────

PRAGMA journal_mode       = WAL;
PRAGMA synchronous        = NORMAL;
PRAGMA busy_timeout       = 5000;
PRAGMA cache_size         = -64000;
PRAGMA temp_store         = MEMORY;
PRAGMA mmap_size          = 268435456;
PRAGMA foreign_keys       = ON;
PRAGMA journal_size_limit = 67108864;

-- ── Core ────────────────────────────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS user (
    id             INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    name           TEXT    NOT NULL,
    last_name      TEXT    NOT NULL,
    role           TEXT    NOT NULL  CHECK (role IN ('owner', 'resident', 'guard', 'guest')),
    is_active      INTEGER NOT NULL  DEFAULT 1  CHECK (is_active IN (0, 1)),
    created_at     INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at     INTEGER,
    deleted_at     INTEGER
);

CREATE TABLE IF NOT EXISTS person (
    id             INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id        INTEGER           REFERENCES user(id) ON DELETE SET NULL,
    name           TEXT    NOT NULL  DEFAULT '',
    alias          TEXT    NOT NULL  DEFAULT '',
    observation    TEXT    NOT NULL  DEFAULT '',
    first_seen_at  INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    last_seen_at   INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    created_at     INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at     INTEGER,
    deleted_at     INTEGER
);

CREATE TABLE IF NOT EXISTS event (
    id          INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    event_type  TEXT    NOT NULL,
    severity    TEXT    NOT NULL  DEFAULT 'info'
                                  CHECK (severity IN ('info', 'warning', 'critical')),
    source      TEXT    NOT NULL  DEFAULT '',
    summary     TEXT    NOT NULL  DEFAULT '',
    details     TEXT    NOT NULL  DEFAULT '{}',
    occurred_at INTEGER NOT NULL,
    created_at  INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at  INTEGER,
    deleted_at  INTEGER
);

CREATE TABLE IF NOT EXISTS person_event (
    person_id  INTEGER NOT NULL  REFERENCES person(id) ON DELETE CASCADE,
    event_id   INTEGER NOT NULL  REFERENCES event(id)  ON DELETE CASCADE,
    confidence REAL    NOT NULL  DEFAULT 0.0,
    PRIMARY KEY (person_id, event_id)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS reminder (
    id              INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    created_by      INTEGER           REFERENCES user(id) ON DELETE SET NULL,
    target_user_id  INTEGER NOT NULL  REFERENCES user(id) ON DELETE CASCADE,
    title           TEXT    NOT NULL,
    description     TEXT    NOT NULL  DEFAULT '',
    scheduled_at    INTEGER NOT NULL,
    recurrence_rule TEXT,
    is_completed    INTEGER NOT NULL  DEFAULT 0  CHECK (is_completed IN (0, 1)),
    completed_at    INTEGER,
    created_at      INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at      INTEGER,
    deleted_at      INTEGER
);

CREATE TABLE IF NOT EXISTS reminder_detail (
    id          INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    reminder_id INTEGER NOT NULL  REFERENCES reminder(id) ON DELETE CASCADE,
    created_by  INTEGER           REFERENCES user(id) ON DELETE SET NULL,
    content     TEXT    NOT NULL,
    status      TEXT    NOT NULL  DEFAULT 'pending'
                                  CHECK (status IN ('pending', 'in_progress', 'done', 'blocked')),
    file_paths  TEXT    NOT NULL  DEFAULT '[]',
    created_at  INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at  INTEGER,
    deleted_at  INTEGER
);

CREATE TABLE IF NOT EXISTS context_note (
    id          INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    created_by  INTEGER           REFERENCES user(id) ON DELETE SET NULL,
    title       TEXT    NOT NULL,
    content     TEXT    NOT NULL  DEFAULT '',
    tags        TEXT    NOT NULL  DEFAULT '',
    valid_from  INTEGER,
    valid_until INTEGER,
    is_active   INTEGER NOT NULL  DEFAULT 1  CHECK (is_active IN (0, 1)),
    created_at  INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at  INTEGER,
    deleted_at  INTEGER
);

CREATE TABLE IF NOT EXISTS schema_version (
    version    INTEGER NOT NULL  PRIMARY KEY,
    applied_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS refresh_token (
    id            INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id       INTEGER NOT NULL  REFERENCES user(id) ON DELETE CASCADE,
    access_token  TEXT    NOT NULL,
    refresh_token TEXT    NOT NULL,
    device_hash   TEXT    NOT NULL,
    user_agent    TEXT    NOT NULL  DEFAULT '',
    is_valid      INTEGER NOT NULL  DEFAULT 1  CHECK (is_valid IN (0, 1)),
    is_used       INTEGER NOT NULL  DEFAULT 0  CHECK (is_used  IN (0, 1)),
    expires_at    INTEGER NOT NULL,
    created_at    INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

-- ── Face Recognition ─────────────────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS face_embedding (
    id          INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    person_id   INTEGER NOT NULL  REFERENCES person(id) ON DELETE CASCADE,
    embedding   BLOB    NOT NULL,
    angle_label TEXT    NOT NULL  DEFAULT 'frontal',
    quality     REAL    NOT NULL  DEFAULT 1.0,
    created_at  INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

-- ── Cameras & Recording ─────────────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS camera (
    id             INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    name           TEXT    NOT NULL,
    manufacturer   TEXT    NOT NULL  DEFAULT '',
    model          TEXT    NOT NULL  DEFAULT '',
    ip             TEXT    NOT NULL,
    port           INTEGER NOT NULL  DEFAULT 554,
    username       TEXT    NOT NULL  DEFAULT 'admin',
    password       TEXT    NOT NULL  DEFAULT '',
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
    points     TEXT    NOT NULL,          -- [ {x, y} ] in [0..1] normalised coords
    zone_type  TEXT    NOT NULL  DEFAULT 'monitor'
                                     CHECK (zone_type IN ('monitor', 'alert', 'exclude')),
    color      TEXT    NOT NULL  DEFAULT '#FF0000',
    is_enabled INTEGER NOT NULL  DEFAULT 1,
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at INTEGER,
    deleted_at INTEGER
);

-- ── Audit & Sync ────────────────────────────────────────────────────────────

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

CREATE TABLE IF NOT EXISTS user_action_log (
    id         INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id    INTEGER NOT NULL  REFERENCES user(id) ON DELETE CASCADE,
    record_id  INTEGER NOT NULL,
    table_name TEXT    NOT NULL,
    action     TEXT    NOT NULL  CHECK (action IN ('create', 'update', 'delete')),
    old_data   TEXT    NOT NULL  DEFAULT '{}',
    new_data   TEXT    NOT NULL  DEFAULT '{}',
    ip_address TEXT    NOT NULL  DEFAULT '',
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

-- ── Indexes ─────────────────────────────────────────────────────────────────
--   Only index columns used in WHERE, JOIN, or ORDER BY.

-- event
CREATE INDEX IF NOT EXISTS idx_event_event_type    ON event (event_type);
CREATE INDEX IF NOT EXISTS idx_event_severity      ON event (severity);
CREATE INDEX IF NOT EXISTS idx_event_occurred_at   ON event (occurred_at);
CREATE INDEX IF NOT EXISTS idx_event_created_at    ON event (created_at);
CREATE INDEX IF NOT EXISTS idx_event_deleted_at    ON event (deleted_at);

-- person_event
CREATE INDEX IF NOT EXISTS idx_person_event_event_id  ON person_event (event_id);

-- reminder
CREATE INDEX IF NOT EXISTS idx_reminder_target_user  ON reminder (target_user_id);
CREATE INDEX IF NOT EXISTS idx_reminder_scheduled    ON reminder (scheduled_at);
CREATE INDEX IF NOT EXISTS idx_reminder_created_at   ON reminder (created_at);
CREATE INDEX IF NOT EXISTS idx_reminder_deleted_at   ON reminder (deleted_at);

-- reminder_detail
CREATE INDEX IF NOT EXISTS idx_reminder_detail_reminder  ON reminder_detail (reminder_id);
CREATE INDEX IF NOT EXISTS idx_reminder_detail_created   ON reminder_detail (created_at);
CREATE INDEX IF NOT EXISTS idx_reminder_detail_deleted   ON reminder_detail (deleted_at);

-- context_note
CREATE INDEX IF NOT EXISTS idx_context_note_created_by  ON context_note (created_by);
CREATE INDEX IF NOT EXISTS idx_context_note_tags        ON context_note (tags);
CREATE INDEX IF NOT EXISTS idx_context_note_created_at  ON context_note (created_at);
CREATE INDEX IF NOT EXISTS idx_context_note_deleted_at  ON context_note (deleted_at);

-- camera
CREATE INDEX IF NOT EXISTS idx_camera_created_at  ON camera (created_at);
CREATE INDEX IF NOT EXISTS idx_camera_deleted_at  ON camera (deleted_at);

-- camera_stream
CREATE INDEX IF NOT EXISTS idx_camera_stream_camera   ON camera_stream (camera_id);
CREATE INDEX IF NOT EXISTS idx_camera_stream_created  ON camera_stream (created_at);
CREATE INDEX IF NOT EXISTS idx_camera_stream_deleted  ON camera_stream (deleted_at);

-- zone
CREATE INDEX IF NOT EXISTS idx_zone_camera_id   ON zone (camera_id);
CREATE INDEX IF NOT EXISTS idx_zone_created_at  ON zone (created_at);
CREATE INDEX IF NOT EXISTS idx_zone_deleted_at  ON zone (deleted_at);

-- face_embedding
CREATE INDEX IF NOT EXISTS idx_face_embedding_person ON face_embedding (person_id);

-- user
CREATE INDEX IF NOT EXISTS idx_user_created_at  ON user (created_at);
CREATE INDEX IF NOT EXISTS idx_user_deleted_at  ON user (deleted_at);

-- refresh_token
CREATE INDEX IF NOT EXISTS idx_refresh_token_user_id   ON refresh_token (user_id);
CREATE INDEX IF NOT EXISTS idx_refresh_token_access    ON refresh_token (access_token);
CREATE INDEX IF NOT EXISTS idx_refresh_token_refresh   ON refresh_token (refresh_token);

-- audit_log
CREATE INDEX IF NOT EXISTS idx_audit_log_record   ON audit_log (record_id, table_name);
CREATE INDEX IF NOT EXISTS idx_audit_log_table_ts ON audit_log (table_name, event_timestamp);

-- user_audit_log
CREATE INDEX IF NOT EXISTS idx_user_audit_log_user_ts ON user_audit_log (user_id, event_timestamp);
CREATE INDEX IF NOT EXISTS idx_user_audit_log_record   ON user_audit_log (record_id, table_name);

-- notification
CREATE INDEX IF NOT EXISTS idx_notification_user_created ON notification (user_id, created_at);

-- notification_token
CREATE UNIQUE INDEX IF NOT EXISTS idx_notification_token_uniq
    ON notification_token (user_id, device_hash);
CREATE INDEX IF NOT EXISTS idx_notification_token_user ON notification_token (user_id);

-- user_action_log
CREATE INDEX IF NOT EXISTS idx_user_action_log_user    ON user_action_log (user_id);
CREATE INDEX IF NOT EXISTS idx_user_action_log_record  ON user_action_log (record_id, table_name);
CREATE INDEX IF NOT EXISTS idx_user_action_log_created ON user_action_log (created_at);
