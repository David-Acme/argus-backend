-- ─────────────────────────────────────────────────────────────────────────────
-- Argus backend  ·  SQLite schema
-- Applied at startup via  DbService::runScriptFile("database/schema.sql")
-- Structure: pragmas → table creation (grouped by domain) → virtual tables →
-- indexes (grouped by table). Keep this order when adding new objects.
-- ─────────────────────────────────────────────────────────────────────────────

PRAGMA journal_mode       = WAL;
PRAGMA synchronous        = NORMAL;
PRAGMA busy_timeout       = 5000;
PRAGMA cache_size         = -64000;
PRAGMA temp_store         = MEMORY;
PRAGMA mmap_size          = 268435456;
PRAGMA foreign_keys       = ON;
PRAGMA journal_size_limit = 67108864;

-- ── Tables · Core ────────────────────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS user (
    id             INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    name           TEXT    NOT NULL,
    last_name      TEXT    NOT NULL,
    role           TEXT    NOT NULL  CHECK (role IN ('owner', 'resident', 'guard', 'guest')),
    lang           TEXT    NOT NULL  DEFAULT 'es'  CHECK (lang IN ('es', 'en')),
    is_active      INTEGER NOT NULL  DEFAULT 1  CHECK (is_active IN (0, 1)),
    created_at     INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at     INTEGER,
    deleted_at     INTEGER
);

-- Invitation tokens are stored only as hashes. A QR code contains the opaque
-- token, while this database can safely retain the invitation audit trail.
CREATE TABLE IF NOT EXISTS user_invitation (
    id                INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    token_hash        TEXT    NOT NULL UNIQUE,
    role              TEXT    NOT NULL CHECK (role IN ('resident', 'guard', 'guest')),
    max_redemptions   INTEGER NOT NULL CHECK (max_redemptions BETWEEN 1 AND 100),
    redemption_count  INTEGER NOT NULL DEFAULT 0
                                CHECK (redemption_count BETWEEN 0 AND max_redemptions),
    expires_at        INTEGER NOT NULL,
    created_by        INTEGER NOT NULL REFERENCES user(id) ON DELETE RESTRICT,
    revoked_at        INTEGER,
    revoked_by        INTEGER REFERENCES user(id) ON DELETE SET NULL,
    created_at        INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    updated_at        INTEGER,
    CHECK (revoked_at IS NULL OR revoked_at >= created_at)
);

-- The unique user key makes an accepted enrollment traceable to exactly one
-- invitation. The service records this in the same transaction as enrollment.
CREATE TABLE IF NOT EXISTS invitation_redemption (
    id              INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    invitation_id   INTEGER NOT NULL REFERENCES user_invitation(id) ON DELETE CASCADE,
    user_id         INTEGER NOT NULL UNIQUE REFERENCES user(id) ON DELETE RESTRICT,
    redeemed_at     INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

-- S3-compatible object metadata. The actual bytes remain private in local
-- object storage and object_key is intentionally never a syncable user field.
CREATE TABLE IF NOT EXISTS stored_file (
    id              INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    object_key      TEXT    NOT NULL UNIQUE,
    sha256          TEXT    NOT NULL,
    mime_type       TEXT    NOT NULL,
    byte_size       INTEGER NOT NULL CHECK (byte_size > 0),
    category        TEXT    NOT NULL CHECK (category IN ('portrait', 'attachment')),
    created_by      INTEGER REFERENCES user(id) ON DELETE SET NULL,
    created_at      INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    deleted_at      INTEGER
);

-- One row is the current private portrait for each user. Re-enrollment uses an
-- upsert, leaving the historical object eligible for storage retention policy.
CREATE TABLE IF NOT EXISTS user_portrait (
    id              INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    user_id         INTEGER NOT NULL UNIQUE REFERENCES user(id) ON DELETE CASCADE,
    file_id         INTEGER NOT NULL UNIQUE REFERENCES stored_file(id) ON DELETE RESTRICT,
    created_at      INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    updated_at      INTEGER
);

-- A preview capability is opaque, short lived, and consumed exactly once.
-- Portrait bytes are only ever served through Argus, never as an object URL.
CREATE TABLE IF NOT EXISTS portrait_preview_capability (
    id                  INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    token_hash          TEXT    NOT NULL UNIQUE,
    portrait_user_id    INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,
    requester_user_id   INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,
    expires_at          INTEGER NOT NULL,
    consumed_at         INTEGER,
    created_at          INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
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

CREATE TABLE IF NOT EXISTS project (
    id          INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    owner_id    INTEGER NOT NULL  REFERENCES user(id) ON DELETE CASCADE,
    name        TEXT    NOT NULL,
    description TEXT    NOT NULL  DEFAULT '',
    status      TEXT    NOT NULL  DEFAULT 'active'
                                  CHECK (status IN ('planned', 'active', 'paused', 'done', 'canceled')),
    color       TEXT    NOT NULL  DEFAULT '',
    starts_at   INTEGER,
    target_at   INTEGER,
    created_at  INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at  INTEGER,
    deleted_at  INTEGER
);

CREATE TABLE IF NOT EXISTS project_task (
    id          INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    project_id  INTEGER NOT NULL  REFERENCES project(id) ON DELETE CASCADE,
    created_by  INTEGER           REFERENCES user(id) ON DELETE SET NULL,
    assignee_id INTEGER           REFERENCES user(id) ON DELETE SET NULL,
    title       TEXT    NOT NULL,
    status      TEXT    NOT NULL  DEFAULT 'todo'
                                  CHECK (status IN ('backlog', 'todo', 'doing', 'done', 'canceled')),
    priority    TEXT    NOT NULL  DEFAULT 'none'
                                  CHECK (priority IN ('none', 'low', 'medium', 'high', 'urgent')),
    due_at      INTEGER,
    -- Float so reordering touches one row instead of rewriting the list.
    sort_order  REAL    NOT NULL  DEFAULT 0,
    created_at  INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at  INTEGER,
    deleted_at  INTEGER
);

CREATE INDEX IF NOT EXISTS idx_project_task_project_status
    ON project_task(project_id, status, sort_order);

CREATE TABLE IF NOT EXISTS calendar_event (
    id              INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    created_by      INTEGER           REFERENCES user(id) ON DELETE SET NULL,
    owner_id        INTEGER NOT NULL  REFERENCES user(id) ON DELETE CASCADE,
    project_id      INTEGER           REFERENCES project(id) ON DELETE SET NULL,
    title           TEXT    NOT NULL,
    description     TEXT    NOT NULL  DEFAULT '',
    location        TEXT    NOT NULL  DEFAULT '',
    color           TEXT    NOT NULL  DEFAULT '',
    starts_at       INTEGER NOT NULL,
    -- NULL means a point in time (a reminder-like mark), not a span.
    ends_at         INTEGER,
    is_all_day      INTEGER NOT NULL  DEFAULT 0  CHECK (is_all_day IN (0, 1)),
    recurrence_rule TEXT,
    created_at      INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at      INTEGER,
    deleted_at      INTEGER
);

CREATE INDEX IF NOT EXISTS idx_calendar_event_owner_start
    ON calendar_event(owner_id, starts_at);

-- ── Tables · Sharing ────────────────────────────────────────────────────────
-- A calendar and a project belong to one user. Sharing is explicit membership
-- with a per-member access level, so "share with Ana, read-only" is a row and
-- not a flag that widens the record to the whole house.

CREATE TABLE IF NOT EXISTS project_member (
    id         INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER NOT NULL  REFERENCES project(id) ON DELETE CASCADE,
    user_id    INTEGER NOT NULL  REFERENCES user(id)    ON DELETE CASCADE,
    access     TEXT    NOT NULL  DEFAULT 'view'  CHECK (access IN ('view', 'edit')),
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at INTEGER,
    deleted_at INTEGER
);

-- Partial unique: a revoked membership stays as history and must not block a
-- later re-share of the same project with the same person.
CREATE UNIQUE INDEX IF NOT EXISTS idx_project_member_unique
    ON project_member(project_id, user_id) WHERE deleted_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_project_member_user
    ON project_member(user_id) WHERE deleted_at IS NULL;

CREATE TABLE IF NOT EXISTS calendar_event_share (
    id               INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    calendar_event_id INTEGER NOT NULL REFERENCES calendar_event(id) ON DELETE CASCADE,
    user_id          INTEGER NOT NULL  REFERENCES user(id)           ON DELETE CASCADE,
    access           TEXT    NOT NULL  DEFAULT 'view'  CHECK (access IN ('view', 'edit')),
    created_at       INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at       INTEGER,
    deleted_at       INTEGER
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_calendar_event_share_unique
    ON calendar_event_share(calendar_event_id, user_id) WHERE deleted_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_calendar_event_share_user
    ON calendar_event_share(user_id) WHERE deleted_at IS NULL;

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

-- ── Tables · Cross-device login (desktop QR) ────────────────────────────────
-- A pending challenge lets a desktop pair with the mobile owner: the desktop
-- creates it, the mobile approves it, the desktop polls it for tokens. Tokens
-- are bound to the desktop's device_hash so its DeviceFilter matches.

CREATE TABLE IF NOT EXISTS device_login_challenge (
    id            INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    challenge_id  TEXT    NOT NULL  UNIQUE,
    device_hash   TEXT    NOT NULL,
    user_agent    TEXT    NOT NULL  DEFAULT '',
    status        TEXT    NOT NULL  DEFAULT 'pending'
                            CHECK (status IN ('pending', 'approved', 'expired')),
    user_id       INTEGER,
    access_token  TEXT,
    refresh_token TEXT,
    expires_at    INTEGER NOT NULL,
    created_at    INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

-- A per-device secret presented via the X-Argus-Device-Credential header
-- (credential identity mode) replaces the source IP in the device hash. Only
-- its SHA-256 is stored; the plaintext is returned once at issuance.
CREATE TABLE IF NOT EXISTS device_credential (
    id            INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id       INTEGER NOT NULL  REFERENCES user(id) ON DELETE CASCADE,
    device_hash   TEXT    NOT NULL,
    secret_hash   TEXT    NOT NULL  UNIQUE,
    is_active     INTEGER NOT NULL  DEFAULT 1  CHECK (is_active IN (0, 1)),
    created_at    INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

-- ── Tables · Face Recognition ────────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS face_embedding (
    id          INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    person_id   INTEGER NOT NULL  REFERENCES person(id) ON DELETE CASCADE,
    embedding   BLOB    NOT NULL,
    angle_label TEXT    NOT NULL  DEFAULT 'frontal',
    quality     REAL    NOT NULL  DEFAULT 1.0,
    created_at  INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

-- ── Tables · Cameras & Recording ─────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS camera (
    id             INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    name           TEXT    NOT NULL,
    manufacturer   TEXT    NOT NULL  DEFAULT '',
    model          TEXT    NOT NULL  DEFAULT '',
    ip             TEXT    NOT NULL,
    port           INTEGER NOT NULL  DEFAULT 554,
    username       TEXT    NOT NULL  DEFAULT 'admin',
    password       TEXT    NOT NULL  DEFAULT '',
    -- The talk channel speaks the vendor cloud protocol, which wants the cloud
    -- account rather than the camera one (see labs/tapo-probe --talk).
    cloud_username TEXT    NOT NULL  DEFAULT '',
    cloud_password TEXT    NOT NULL  DEFAULT '',
    driver         TEXT    NOT NULL  DEFAULT 'tapo'
                                     CHECK (driver IN ('tapo', 'onvif', 'rtsp')),
    -- Icon key from the client registry, so a camera looks the same everywhere.
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
    points     TEXT    NOT NULL,          -- [ {x, y} ] in [0..1] normalised coords
    zone_type  TEXT    NOT NULL  DEFAULT 'monitor'
                                     CHECK (zone_type IN ('monitor', 'alert', 'exclude')),
    color      TEXT    NOT NULL  DEFAULT '#FF0000',
    is_enabled INTEGER NOT NULL  DEFAULT 1,
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at INTEGER,
    deleted_at INTEGER
);

-- ── Tables · Audit & Sync ────────────────────────────────────────────────────

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
    action     TEXT    NOT NULL  CHECK (action IN ('create', 'read', 'update', 'delete')),
    old_data   TEXT    NOT NULL  DEFAULT '{}',
    new_data   TEXT    NOT NULL  DEFAULT '{}',
    ip_address TEXT    NOT NULL  DEFAULT '',
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

-- ── Tables · Memory graph ────────────────────────────────────────────────────
-- Entity-anchored facts, alias frames, typed edges
-- (about/related/supersedes/mentions/derived/rolled_up), episodic memory and
-- provenance. Embeddings reuse memory_vec (vec0) with partitions
-- 'graph:fact:<scope>' / 'graph:episode:<scope>'.

CREATE TABLE IF NOT EXISTS memory_entity (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  kind TEXT NOT NULL CHECK (kind IN ('person', 'place', 'device', 'pet', 'thing', 'concept')),
  canonical TEXT NOT NULL,
  lang TEXT NOT NULL DEFAULT 'es',
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  person_id INTEGER
);

CREATE TABLE IF NOT EXISTS memory_alias (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  entity_id INTEGER NOT NULL REFERENCES memory_entity(id) ON DELETE CASCADE,
  surface TEXT NOT NULL,
  norm TEXT NOT NULL,
  lang TEXT NOT NULL DEFAULT 'es',
  person_frame TEXT NOT NULL CHECK (person_frame IN ('first', 'second', 'none')),
  confidence REAL NOT NULL DEFAULT 0.9
);

CREATE TABLE IF NOT EXISTS memory_fact (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  entity_id INTEGER NOT NULL REFERENCES memory_entity(id) ON DELETE CASCADE,
  predicate TEXT NOT NULL,
  value TEXT NOT NULL,
  canonical TEXT NOT NULL,
  type TEXT NOT NULL CHECK (type IN ('persona', 'preference', 'schedule', 'instruction', 'attribute')),
  priority INTEGER NOT NULL DEFAULT 50,
  confidence REAL NOT NULL DEFAULT 0.9,
  lang TEXT NOT NULL DEFAULT 'es',
  scope TEXT NOT NULL DEFAULT 'user',
  ref_id INTEGER NOT NULL DEFAULT 0,
  valid_from INTEGER NOT NULL,
  valid_to INTEGER NOT NULL DEFAULT 0,
  hit_count INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS memory_edge (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  kind TEXT NOT NULL CHECK (kind IN ('about', 'related', 'mentions', 'supersedes', 'derived', 'rolled_up')),
  src_id INTEGER NOT NULL,
  dst_id INTEGER NOT NULL,
  predicate TEXT,
  since INTEGER NOT NULL DEFAULT 0,
  until INTEGER NOT NULL DEFAULT 0,
  ord INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS memory_episode (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  kind TEXT NOT NULL,
  summary TEXT NOT NULL,
  actor TEXT,
  occurred_at INTEGER NOT NULL,
  session_id TEXT,
  lang TEXT NOT NULL DEFAULT 'es',
  scope TEXT NOT NULL DEFAULT 'user',
  ref_id INTEGER NOT NULL DEFAULT 0,
  salience REAL NOT NULL DEFAULT 0.5,
  hit_count INTEGER NOT NULL DEFAULT 0,
  last_recalled_at INTEGER NOT NULL DEFAULT 0,
  decayed_at INTEGER NOT NULL DEFAULT 0,
  rolled_up INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS memory_source (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  channel TEXT NOT NULL,
  turn_ref TEXT,
  at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS memory_procedure (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL,
  goal TEXT NOT NULL,
  steps TEXT NOT NULL,
  uses INTEGER NOT NULL DEFAULT 0,
  successes INTEGER NOT NULL DEFAULT 0,
  updated_at INTEGER NOT NULL
);

-- ── Virtual tables · Memory FTS5 (external content) ─────────────────────────

CREATE VIRTUAL TABLE IF NOT EXISTS memory_fact_fts USING fts5(
  canonical, content = 'memory_fact', content_rowid = 'id',
  tokenize = 'unicode61 remove_diacritics 2'
);

CREATE VIRTUAL TABLE IF NOT EXISTS memory_episode_fts USING fts5(
  summary, content = 'memory_episode', content_rowid = 'id',
  tokenize = 'unicode61 remove_diacritics 2'
);

CREATE VIRTUAL TABLE IF NOT EXISTS memory_alias_fts USING fts5(
  norm, content = 'memory_alias', content_rowid = 'id',
  tokenize = 'unicode61 remove_diacritics 2'
);

-- ── Indexes ──────────────────────────────────────────────────────────────────
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

-- user_invitation
CREATE UNIQUE INDEX IF NOT EXISTS idx_user_invitation_token_hash
    ON user_invitation (token_hash);
CREATE INDEX IF NOT EXISTS idx_user_invitation_active
    ON user_invitation (expires_at, revoked_at);
CREATE INDEX IF NOT EXISTS idx_user_invitation_creator
    ON user_invitation (created_by, created_at DESC);

-- invitation_redemption
CREATE INDEX IF NOT EXISTS idx_invitation_redemption_invitation
    ON invitation_redemption (invitation_id, redeemed_at DESC);
CREATE UNIQUE INDEX IF NOT EXISTS idx_invitation_redemption_user
    ON invitation_redemption (user_id);

-- stored_file
CREATE UNIQUE INDEX IF NOT EXISTS idx_stored_file_object_key
    ON stored_file (object_key);
CREATE INDEX IF NOT EXISTS idx_stored_file_category_created
    ON stored_file (category, created_at DESC) WHERE deleted_at IS NULL;

-- user_portrait
CREATE UNIQUE INDEX IF NOT EXISTS idx_user_portrait_user_current
    ON user_portrait (user_id);
CREATE INDEX IF NOT EXISTS idx_portrait_preview_capability_lookup
    ON portrait_preview_capability (token_hash, expires_at, consumed_at);

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

-- memory_fact
CREATE INDEX IF NOT EXISTS idx_memory_fact_entity ON memory_fact (entity_id, predicate);
CREATE INDEX IF NOT EXISTS idx_memory_fact_scope  ON memory_fact (scope, ref_id);
CREATE INDEX IF NOT EXISTS idx_memory_fact_valid  ON memory_fact (valid_to);

-- memory_alias
CREATE INDEX IF NOT EXISTS idx_memory_alias_norm  ON memory_alias (norm);
CREATE INDEX IF NOT EXISTS idx_memory_alias_entity ON memory_alias (entity_id);

-- memory_edge
CREATE INDEX IF NOT EXISTS idx_memory_edge_kind   ON memory_edge (kind, src_id);
CREATE INDEX IF NOT EXISTS idx_memory_edge_dst    ON memory_edge (kind, dst_id);

-- memory_episode
CREATE INDEX IF NOT EXISTS idx_memory_episode_time ON memory_episode (occurred_at);
CREATE INDEX IF NOT EXISTS idx_memory_episode_scope ON memory_episode (scope, ref_id);

