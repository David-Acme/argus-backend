-- Argus backend SQLite schema
-- Applied at startup via DbService::runScriptFile("database/schema.sql").
--
-- Naming convention:
--   pk_{table}              PRIMARY KEY (inline)
--   fk_{table}_{column}     FOREIGN KEY (inline)
--   uq_{table}_{column}     UNIQUE (inline, creates auto index)
--   ck_{table}_{column}     CHECK (inline)
--   idx_{table}_{column}    CREATE INDEX (non-unique, separate statement)
--
-- Performance notes:
--   • Index only what appears in WHERE, JOIN, or ORDER BY clauses.
--   • INTERSECT for concise AND operations across indexed columns.
--   • DELETE + INSERT beats UPDATE when data changes often (avoids B-tree fragmentation).

PRAGMA journal_mode       = WAL;
PRAGMA synchronous        = NORMAL;
PRAGMA busy_timeout       = 5000;
PRAGMA cache_size         = -64000;
PRAGMA temp_store         = MEMORY;
PRAGMA mmap_size          = 268435456;
PRAGMA foreign_keys       = ON;
PRAGMA journal_size_limit = 67108864;

-- ⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄
-- TABLES

CREATE TABLE IF NOT EXISTS user (
    id              INTEGER NOT NULL CONSTRAINT pk_user PRIMARY KEY AUTOINCREMENT,
    feature_hub_id  INTEGER NOT NULL CONSTRAINT uq_user_feature_hub_id UNIQUE,
    name            TEXT    NOT NULL,
    last_name       TEXT    NOT NULL,
    role            TEXT    NOT NULL CONSTRAINT ck_user_role CHECK (role IN ('owner', 'resident', 'guard', 'guest')),
    is_active       INTEGER NOT NULL DEFAULT 1 CONSTRAINT ck_user_is_active CHECK (is_active IN (0, 1)),
    created_at      INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    updated_at      INTEGER NULL,
    deleted_at      INTEGER NULL
);

CREATE TABLE IF NOT EXISTS person (
    id              INTEGER NOT NULL CONSTRAINT pk_person PRIMARY KEY AUTOINCREMENT,
    feature_hub_id  INTEGER NOT NULL CONSTRAINT uq_person_feature_hub_id UNIQUE,
    name            TEXT    NOT NULL DEFAULT '',
    alias           TEXT    NOT NULL DEFAULT '',
    observation     TEXT    NOT NULL DEFAULT '',
    first_seen_at   INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    last_seen_at    INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    created_at      INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    updated_at      INTEGER NULL,
    deleted_at      INTEGER NULL
);

CREATE TABLE IF NOT EXISTS event (
    id          INTEGER NOT NULL CONSTRAINT pk_event PRIMARY KEY AUTOINCREMENT,
    event_type  TEXT    NOT NULL,
    severity    TEXT    NOT NULL DEFAULT 'info' CONSTRAINT ck_event_severity CHECK (severity IN ('info', 'warning', 'critical')),
    source      TEXT    NOT NULL DEFAULT '',
    summary     TEXT    NOT NULL DEFAULT '',
    details     TEXT    NOT NULL DEFAULT '{}',
    occurred_at INTEGER NOT NULL,
    created_at  INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    updated_at  INTEGER NULL,
    deleted_at  INTEGER NULL
);

CREATE TABLE IF NOT EXISTS person_event (
    person_id   INTEGER NOT NULL CONSTRAINT fk_person_event_person_id REFERENCES person(id) ON DELETE CASCADE,
    event_id    INTEGER NOT NULL CONSTRAINT fk_person_event_event_id REFERENCES event(id) ON DELETE CASCADE,
    confidence  REAL    NOT NULL DEFAULT 0.0,
    CONSTRAINT pk_person_event PRIMARY KEY (person_id, event_id)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS reminder (
    id               INTEGER NOT NULL CONSTRAINT pk_reminder PRIMARY KEY AUTOINCREMENT,
    created_by       INTEGER NULL CONSTRAINT fk_reminder_created_by REFERENCES user(id) ON DELETE SET NULL,
    target_user_id   INTEGER NOT NULL CONSTRAINT fk_reminder_target_user_id REFERENCES user(id) ON DELETE CASCADE,
    title            TEXT    NOT NULL,
    description      TEXT    NOT NULL DEFAULT '',
    scheduled_at     INTEGER NOT NULL,
    recurrence_rule  TEXT    NULL,
    is_completed     INTEGER NOT NULL DEFAULT 0 CONSTRAINT ck_reminder_is_completed CHECK (is_completed IN (0, 1)),
    completed_at     INTEGER NULL,
    created_at       INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    updated_at       INTEGER NULL,
    deleted_at       INTEGER NULL
);

CREATE TABLE IF NOT EXISTS reminder_detail (
    id           INTEGER NOT NULL CONSTRAINT pk_reminder_detail PRIMARY KEY AUTOINCREMENT,
    reminder_id  INTEGER NOT NULL CONSTRAINT fk_reminder_detail_reminder_id REFERENCES reminder(id) ON DELETE CASCADE,
    created_by   INTEGER NULL CONSTRAINT fk_reminder_detail_created_by REFERENCES user(id) ON DELETE SET NULL,
    content      TEXT    NOT NULL,
    status       TEXT    NOT NULL DEFAULT 'pending' CONSTRAINT ck_reminder_detail_status CHECK (status IN ('pending', 'in_progress', 'done', 'blocked')),
    file_paths   TEXT    NOT NULL DEFAULT '[]',
    created_at   INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    updated_at   INTEGER NULL,
    deleted_at   INTEGER NULL
);

CREATE TABLE IF NOT EXISTS context_note (
    id          INTEGER NOT NULL CONSTRAINT pk_context_note PRIMARY KEY AUTOINCREMENT,
    created_by  INTEGER NULL CONSTRAINT fk_context_note_created_by REFERENCES user(id) ON DELETE SET NULL,
    title       TEXT    NOT NULL,
    content     TEXT    NOT NULL DEFAULT '',
    tags        TEXT    NOT NULL DEFAULT '',
    valid_from  INTEGER NULL,
    valid_until INTEGER NULL,
    is_active   INTEGER NOT NULL DEFAULT 1 CONSTRAINT ck_context_note_is_active CHECK (is_active IN (0, 1)),
    created_at  INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    updated_at  INTEGER NULL,
    deleted_at  INTEGER NULL
);

CREATE TABLE IF NOT EXISTS schema_version (
    version     INTEGER NOT NULL CONSTRAINT pk_schema_version PRIMARY KEY,
    applied_at  INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

-- ⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄
-- CAMERAS & RECORDING

CREATE TABLE IF NOT EXISTS camera (
    id              INTEGER NOT NULL CONSTRAINT pk_camera PRIMARY KEY AUTOINCREMENT,
    name            TEXT    NOT NULL,
    manufacturer    TEXT    NOT NULL DEFAULT '',
    model           TEXT    NOT NULL DEFAULT '',
    ip              TEXT    NOT NULL,
    port            INTEGER NOT NULL DEFAULT 554,
    username        TEXT    NOT NULL DEFAULT 'admin',
    password        TEXT    NOT NULL DEFAULT '',
    record_mode     TEXT    NOT NULL DEFAULT 'events' CONSTRAINT ck_camera_record_mode CHECK (record_mode IN ('events', 'continuous')),
    retention_days  INTEGER NULL,
    capabilities    TEXT    NOT NULL DEFAULT '[]',
    config          TEXT    NOT NULL DEFAULT '{}',
    is_enabled      INTEGER NOT NULL DEFAULT 1 CONSTRAINT ck_camera_is_enabled CHECK (is_enabled IN (0, 1)),
    is_online       INTEGER NOT NULL DEFAULT 0,
    created_at      INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    updated_at      INTEGER NULL,
    deleted_at      INTEGER NULL
);

CREATE TABLE IF NOT EXISTS camera_stream (
    id           INTEGER NOT NULL CONSTRAINT pk_camera_stream PRIMARY KEY AUTOINCREMENT,
    camera_id    INTEGER NOT NULL CONSTRAINT fk_camera_stream_camera_id REFERENCES camera(id) ON DELETE CASCADE,
    label        TEXT    NOT NULL DEFAULT '',
    url          TEXT    NOT NULL,
    resolution   TEXT    NOT NULL DEFAULT '',
    fps          INTEGER NOT NULL DEFAULT 0,
    codec        TEXT    NOT NULL DEFAULT '',
    is_primary   INTEGER NOT NULL DEFAULT 1,
    is_enabled   INTEGER NOT NULL DEFAULT 1,
    created_at   INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    updated_at   INTEGER NULL,
    deleted_at   INTEGER NULL
);

CREATE TABLE IF NOT EXISTS zone (
    id          INTEGER NOT NULL CONSTRAINT pk_zone PRIMARY KEY AUTOINCREMENT,
    camera_id   INTEGER NOT NULL CONSTRAINT fk_zone_camera_id REFERENCES camera(id) ON DELETE CASCADE,
    name        TEXT    NOT NULL,
    points      TEXT    NOT NULL,                                     -- [{x, y}] in [0..1] normalized coords
    zone_type   TEXT    NOT NULL DEFAULT 'monitor' CONSTRAINT ck_zone_zone_type CHECK (zone_type IN ('monitor', 'alert', 'exclude')),
    color       TEXT    NOT NULL DEFAULT '#FF0000',
    is_enabled  INTEGER NOT NULL DEFAULT 1,
    created_at  INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    updated_at  INTEGER NULL,
    deleted_at  INTEGER NULL
);

-- ⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄
-- AUDIT & SYNC

CREATE TABLE IF NOT EXISTS audit_log (
    id               INTEGER NOT NULL CONSTRAINT pk_audit_log PRIMARY KEY AUTOINCREMENT,
    record_id        INTEGER NOT NULL,
    table_name       TEXT    NOT NULL,
    event_timestamp  INTEGER NOT NULL,
    old_data         TEXT    NOT NULL DEFAULT '{}',
    new_data         TEXT    NOT NULL DEFAULT '{}',
    created_at       INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS user_action_log (
    id           INTEGER NOT NULL CONSTRAINT pk_user_action_log PRIMARY KEY AUTOINCREMENT,
    user_id      INTEGER NOT NULL CONSTRAINT fk_user_action_log_user_id REFERENCES user(id) ON DELETE CASCADE,
    record_id    INTEGER NOT NULL,
    table_name   TEXT    NOT NULL,
    action       TEXT    NOT NULL CONSTRAINT ck_user_action_log_action CHECK (action IN ('create', 'update', 'delete')),
    old_data     TEXT    NOT NULL DEFAULT '{}',
    new_data     TEXT    NOT NULL DEFAULT '{}',
    ip_address   TEXT    NOT NULL DEFAULT '',
    created_at   INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

-- Tables enabled for sync: camera, camera_stream, zone, reminder,
-- reminder_detail, user.

-- ⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄⌄
-- INDEXES

CREATE INDEX IF NOT EXISTS idx_event_event_type    ON event(event_type);
CREATE INDEX IF NOT EXISTS idx_event_severity      ON event(severity);
CREATE INDEX IF NOT EXISTS idx_event_occurred_at   ON event(occurred_at);
CREATE INDEX IF NOT EXISTS idx_event_created_at    ON event(created_at);
CREATE INDEX IF NOT EXISTS idx_event_deleted_at    ON event(deleted_at);
CREATE INDEX IF NOT EXISTS idx_person_event_event_id ON person_event(event_id);

CREATE INDEX IF NOT EXISTS idx_reminder_target_user ON reminder(target_user_id);
CREATE INDEX IF NOT EXISTS idx_reminder_scheduled   ON reminder(scheduled_at);
CREATE INDEX IF NOT EXISTS idx_reminder_created_at  ON reminder(created_at);
CREATE INDEX IF NOT EXISTS idx_reminder_deleted_at  ON reminder(deleted_at);

CREATE INDEX IF NOT EXISTS idx_reminder_detail_reminder ON reminder_detail(reminder_id);
CREATE INDEX IF NOT EXISTS idx_reminder_detail_created  ON reminder_detail(created_at);
CREATE INDEX IF NOT EXISTS idx_reminder_detail_deleted  ON reminder_detail(deleted_at);

CREATE INDEX IF NOT EXISTS idx_context_note_created_by ON context_note(created_by);
CREATE INDEX IF NOT EXISTS idx_context_note_tags       ON context_note(tags);
CREATE INDEX IF NOT EXISTS idx_context_note_created_at ON context_note(created_at);
CREATE INDEX IF NOT EXISTS idx_context_note_deleted_at ON context_note(deleted_at);

CREATE INDEX IF NOT EXISTS idx_camera_created_at  ON camera(created_at);
CREATE INDEX IF NOT EXISTS idx_camera_deleted_at  ON camera(deleted_at);

CREATE INDEX IF NOT EXISTS idx_camera_stream_camera   ON camera_stream(camera_id);
CREATE INDEX IF NOT EXISTS idx_camera_stream_created  ON camera_stream(created_at);
CREATE INDEX IF NOT EXISTS idx_camera_stream_deleted  ON camera_stream(deleted_at);

CREATE INDEX IF NOT EXISTS idx_zone_camera_id  ON zone(camera_id);
CREATE INDEX IF NOT EXISTS idx_zone_created_at ON zone(created_at);
CREATE INDEX IF NOT EXISTS idx_zone_deleted_at ON zone(deleted_at);

CREATE INDEX IF NOT EXISTS idx_user_created_at ON user(created_at);
CREATE INDEX IF NOT EXISTS idx_user_deleted_at ON user(deleted_at);

CREATE INDEX IF NOT EXISTS idx_audit_log_record   ON audit_log(record_id, table_name);
CREATE INDEX IF NOT EXISTS idx_audit_log_table    ON audit_log(table_name, event_timestamp);
CREATE INDEX IF NOT EXISTS idx_audit_log_created  ON audit_log(created_at);

CREATE INDEX IF NOT EXISTS idx_user_action_log_user    ON user_action_log(user_id);
CREATE INDEX IF NOT EXISTS idx_user_action_log_record  ON user_action_log(record_id, table_name);
CREATE INDEX IF NOT EXISTS idx_user_action_log_created ON user_action_log(created_at);
