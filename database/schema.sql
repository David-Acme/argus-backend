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

-- ---------------------------------------------------------------------------
-- memory graph (SemanticGraph, COGNITIVE_MEMORY_PLAN.md §4 — relational
-- fallback after the Kùzu gate failed). Entity-anchored facts, alias frames,
-- typed edges (about/related/supersedes/mentions/derived/rolled_up),
-- episodic memory and provenance. Embeddings reuse memory_vec (vec0) with
-- partitions 'graph:fact:<scope>' / 'graph:episode:<scope>'.
-- ---------------------------------------------------------------------------
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

CREATE INDEX IF NOT EXISTS idx_memory_fact_entity ON memory_fact (entity_id, predicate);
CREATE INDEX IF NOT EXISTS idx_memory_fact_scope  ON memory_fact (scope, ref_id);
CREATE INDEX IF NOT EXISTS idx_memory_fact_valid  ON memory_fact (valid_to);
CREATE INDEX IF NOT EXISTS idx_memory_alias_norm  ON memory_alias (norm);
CREATE INDEX IF NOT EXISTS idx_memory_alias_entity ON memory_alias (entity_id);
CREATE INDEX IF NOT EXISTS idx_memory_edge_kind   ON memory_edge (kind, src_id);
CREATE INDEX IF NOT EXISTS idx_memory_edge_dst    ON memory_edge (kind, dst_id);
CREATE INDEX IF NOT EXISTS idx_memory_episode_time ON memory_episode (occurred_at);
CREATE INDEX IF NOT EXISTS idx_memory_episode_scope ON memory_episode (scope, ref_id);

CREATE TABLE IF NOT EXISTS job (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  queue TEXT NOT NULL,
  payload TEXT NOT NULL,
  state TEXT NOT NULL CHECK (state IN ('waiting','active','completed','failed','delayed')),
  priority INTEGER NOT NULL DEFAULT 0,
  attempts INTEGER NOT NULL DEFAULT 0,
  max_attempts INTEGER NOT NULL DEFAULT 3,
  dedupe_key TEXT,
  last_error TEXT,
  next_run_at INTEGER,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_job_pick ON job (queue, state, next_run_at, priority DESC);
CREATE UNIQUE INDEX IF NOT EXISTS idx_job_dedupe ON job (queue, dedupe_key) WHERE dedupe_key IS NOT NULL;

CREATE TABLE IF NOT EXISTS memory_phrase (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  kind TEXT NOT NULL CHECK (kind IN ('trigger', 'confirmation', 'statement_start', 'recall_marker', 'interrogative', 'filler')),
  lang TEXT NOT NULL DEFAULT 'es',
  phrase TEXT NOT NULL,
  memory_type TEXT NOT NULL DEFAULT 'persona' CHECK (memory_type IN ('persona', 'episodic', 'instruction', 'system'))
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_memory_phrase_unique ON memory_phrase (kind, lang, phrase);

INSERT OR IGNORE INTO memory_phrase (kind, lang, phrase, memory_type) VALUES
  ('trigger', 'es', 'recuérdame que', 'persona'),
  ('trigger', 'es', 'me hagas recordar que', 'persona'),
  ('trigger', 'es', 'me hagas recordar acerca de que', 'persona'),
  ('trigger', 'es', 'hazme recordar que', 'persona'),
  ('trigger', 'es', 'hazme recordar acerca de que', 'persona'),
  ('trigger', 'es', 'quiero que me hagas recordar que', 'persona'),
  ('trigger', 'es', 'quiero que me hagas recordar acerca de que', 'persona'),
  ('trigger', 'es', 'quisiera que me hagas recordar que', 'persona'),
  ('trigger', 'es', 'quisiera que me hagas recordar acerca de que', 'persona'),
  ('trigger', 'es', 'recuerda que', 'persona'),
  ('trigger', 'es', 'ten en cuenta que', 'persona'),
  ('trigger', 'es', 'no olvides que', 'persona'),
  ('trigger', 'es', 'apunta que', 'episodic'),
  ('trigger', 'es', 'a partir de ahora', 'instruction'),
  ('trigger', 'es', 'de ahora en adelante', 'instruction'),
  ('trigger', 'en', 'remind me that', 'persona'),
  ('trigger', 'en', 'make me remember that', 'persona'),
  ('trigger', 'en', 'can you remember that', 'persona'),
  ('trigger', 'en', 'i want you to remember that', 'persona'),
  ('trigger', 'en', 'remember that', 'persona'),
  ('trigger', 'en', 'keep in mind that', 'persona'),
  ('trigger', 'en', 'don''t forget that', 'persona'),
  ('trigger', 'en', 'note that', 'episodic'),
  ('trigger', 'en', 'from now on', 'instruction'),
  ('confirmation', 'es', '¿está bien?', 'persona'),
  ('confirmation', 'es', '¿esta bien?', 'persona'),
  ('confirmation', 'es', '¿ok?', 'persona'),
  ('confirmation', 'es', '¿verdad?', 'persona'),
  ('confirmation', 'es', '¿no?', 'persona'),
  ('confirmation', 'es', 'ok', 'persona'),
  ('confirmation', 'en', 'right?', 'persona'),
  ('confirmation', 'en', 'ok?', 'persona'),
  ('confirmation', 'en', 'ok', 'persona'),
  ('statement_start', 'es', 'mi hermana', 'persona'),
  ('statement_start', 'es', 'mi hermano', 'persona'),
  ('statement_start', 'es', 'mi mamá', 'persona'),
  ('statement_start', 'es', 'mi papá', 'persona'),
  ('statement_start', 'es', 'mi abuela', 'persona'),
  ('statement_start', 'es', 'mi abuelo', 'persona'),
  ('statement_start', 'es', 'mi esposa', 'persona'),
  ('statement_start', 'es', 'mi marido', 'persona'),
  ('statement_start', 'es', 'mi mujer', 'persona'),
  ('statement_start', 'es', 'el perro', 'persona'),
  ('statement_start', 'es', 'el gato', 'persona'),
  ('statement_start', 'es', 'la alarma', 'persona'),
  ('statement_start', 'es', 'el wifi', 'persona'),
  ('statement_start', 'es', 'el termostato', 'persona'),
  ('statement_start', 'es', 'el garaje', 'persona'),
  ('statement_start', 'es', 'la puerta', 'persona'),
  ('statement_start', 'es', 'la cámara', 'persona'),
  ('statement_start', 'es', 'la reunión', 'episodic'),
  ('statement_start', 'es', 'los niños', 'persona'),
  ('statement_start', 'es', 'el técnico', 'episodic'),
  ('statement_start', 'es', 'el cartero', 'episodic'),
  ('statement_start', 'es', 'mi familia', 'persona'),
  ('statement_start', 'en', 'my sister', 'persona'),
  ('statement_start', 'en', 'my brother', 'persona'),
  ('statement_start', 'en', 'my mom', 'persona'),
  ('statement_start', 'en', 'my dad', 'persona'),
  ('statement_start', 'en', 'my grandmother', 'persona'),
  ('statement_start', 'en', 'my grandfather', 'persona'),
  ('statement_start', 'en', 'my wife', 'persona'),
  ('statement_start', 'en', 'my husband', 'persona'),
  ('statement_start', 'en', 'the dog', 'persona'),
  ('statement_start', 'en', 'the cat', 'persona'),
  ('statement_start', 'en', 'the alarm', 'persona'),
  ('statement_start', 'en', 'the wifi', 'persona'),
  ('statement_start', 'en', 'the thermostat', 'persona'),
  ('statement_start', 'en', 'the garage', 'persona'),
  ('statement_start', 'en', 'the door', 'persona'),
  ('statement_start', 'en', 'the camera', 'persona'),
  ('statement_start', 'en', 'the meeting', 'episodic'),
  ('statement_start', 'en', 'the kids', 'persona'),
  ('statement_start', 'en', 'the technician', 'episodic'),
  ('statement_start', 'en', 'the mailman', 'episodic'),
  ('statement_start', 'en', 'my family', 'persona'),
  ('recall_marker', 'es', 'no recuerdo', 'persona'),
  ('recall_marker', 'es', 'no me acuerdo', 'persona'),
  ('recall_marker', 'es', 'me acuerdo', 'persona'),
  ('recall_marker', 'es', 'recuerdas', 'persona'),
  ('recall_marker', 'es', 'te acuerdas', 'persona'),
  ('recall_marker', 'es', 'te acordaste', 'persona'),
  ('recall_marker', 'es', 'quiero saber', 'persona'),
  ('recall_marker', 'es', 'necesito saber', 'persona'),
  ('recall_marker', 'es', 'quisiera saber', 'persona'),
  ('recall_marker', 'es', 'me gustaría saber', 'persona'),
  ('recall_marker', 'es', 'puedes decirme', 'persona'),
  ('recall_marker', 'es', 'me puedes decir', 'persona'),
  ('recall_marker', 'es', 'me puedes recordar', 'persona'),
  ('recall_marker', 'es', 'dime ', 'persona'),
  ('recall_marker', 'es', 'cuéntame ', 'persona'),
  ('recall_marker', 'es', 'cómo era', 'persona'),
  ('recall_marker', 'es', 'qué era', 'persona'),
  ('recall_marker', 'es', 'cómo se llama', 'persona'),
  ('recall_marker', 'es', 'cuál es el nombre', 'persona'),
  ('recall_marker', 'es', 'qué me dijiste', 'persona'),
  ('recall_marker', 'es', 'qué me contaste', 'persona'),
  ('recall_marker', 'es', 'qué me platicaste', 'persona'),
  ('recall_marker', 'es', 'sabes cuándo', 'persona'),
  ('recall_marker', 'es', 'sabes si', 'persona'),
  ('recall_marker', 'es', 'sabes dónde', 'persona'),
  ('recall_marker', 'es', 'sabes quién', 'persona'),
  ('recall_marker', 'es', 'sabes qué', 'persona'),
  ('recall_marker', 'es', 'sabes cómo', 'persona'),
  ('recall_marker', 'es', 'sabes a qué', 'persona'),
  ('recall_marker', 'es', 'guardaste', 'persona'),
  ('recall_marker', 'es', 'anotaste', 'persona'),
  ('recall_marker', 'es', 'apuntaste', 'persona'),
  ('recall_marker', 'es', 'en qué quedamos', 'persona'),
  ('recall_marker', 'es', 'qué pasó', 'persona'),
  ('recall_marker', 'es', 'qué hago', 'persona'),
  ('recall_marker', 'es', 'dónde está', 'persona'),
  ('recall_marker', 'es', 'dónde están', 'persona'),
  ('recall_marker', 'es', 'dónde dejé', 'persona'),
  ('recall_marker', 'es', 'dónde puse', 'persona'),
  ('recall_marker', 'es', 'cuándo era', 'persona'),
  ('recall_marker', 'es', 'cuándo fue', 'persona'),
  ('recall_marker', 'es', 'a qué hora', 'persona'),
  ('recall_marker', 'es', 'qué día', 'persona'),
  ('recall_marker', 'es', 'qué tengo', 'persona'),
  ('recall_marker', 'es', 'qué necesito', 'persona'),
  ('recall_marker', 'es', 'no sé si', 'persona'),
  ('recall_marker', 'es', 'no sé qué', 'persona'),
  ('recall_marker', 'es', 'no sé que', 'persona');

CREATE TABLE IF NOT EXISTS memory_lexicon (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  kind TEXT NOT NULL CHECK (kind IN ('predicate', 'kinship', 'first_person', 'stopword')),
  lang TEXT NOT NULL DEFAULT 'es',
  surface TEXT NOT NULL,
  canonical TEXT NOT NULL DEFAULT ''
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_memory_lexicon_unique ON memory_lexicon (kind, lang, surface);

INSERT OR IGNORE INTO memory_lexicon (kind, lang, surface, canonical) VALUES
  ('predicate', 'es', 'me gusta', 'prefers'),
  ('predicate', 'es', 'no me gusta', 'dislikes'),
  ('predicate', 'es', 'le gusta', 'likes'),
  ('predicate', 'es', 'no le gusta', 'dislikes'),
  ('predicate', 'es', 'prefiere', 'prefers'),
  ('predicate', 'es', 'trabaja desde casa', 'works_from'),
  ('predicate', 'es', 'viene a comer', 'visits'),
  ('predicate', 'es', 'viene cada', 'visits'),
  ('predicate', 'es', 'viene de visita', 'visits'),
  ('predicate', 'es', 'visita', 'visits'),
  ('predicate', 'es', 'viene', 'visits'),
  ('predicate', 'es', 'es alergico a', 'allergic_to'),
  ('predicate', 'es', 'es alergica a', 'allergic_to'),
  ('predicate', 'es', 'tiene alergia a', 'allergic_to'),
  ('predicate', 'es', 'estudia', 'studies'),
  ('predicate', 'es', 'trabaja', 'works'),
  ('predicate', 'es', 'duerme', 'sleeps_in'),
  ('predicate', 'es', 'cocina', 'cooks'),
  ('predicate', 'es', 'se sirve', 'served_at'),
  ('predicate', 'es', 'se activa', 'activates_at'),
  ('predicate', 'es', 'se cierra', 'closes_at'),
  ('predicate', 'es', 'recoge el correo', 'picks_up_mail'),
  ('predicate', 'es', 'se abre con', 'opens_with'),
  ('predicate', 'es', 'se riega', 'watered_at'),
  ('predicate', 'es', 'se carga', 'charges_at'),
  ('predicate', 'es', 'se pinta', 'painted_in'),
  ('predicate', 'es', 'se pone a', 'set_to'),
  ('predicate', 'es', 'suena cuando', 'beeps_when'),
  ('predicate', 'es', 'come a las', 'eats_at'),
  ('predicate', 'es', 'sale al', 'goes_to'),
  ('predicate', 'es', 'sale a', 'goes_out'),
  ('predicate', 'es', 'abre con', 'opens_with'),
  ('predicate', 'es', 'se llama', 'is'),
  ('predicate', 'es', 'es', 'is'),
  ('predicate', 'en', 'likes', 'likes'),
  ('predicate', 'en', 'does not like', 'dislikes'),
  ('predicate', 'en', 'do not like', 'dislikes'),
  ('predicate', 'en', 'prefers', 'prefers'),
  ('predicate', 'en', 'comes every', 'visits'),
  ('predicate', 'en', 'comes on', 'visits'),
  ('predicate', 'en', 'works from home', 'works_from'),
  ('predicate', 'en', 'comes to visit', 'visits'),
  ('predicate', 'en', 'comes', 'visits'),
  ('predicate', 'en', 'visits', 'visits'),
  ('predicate', 'en', 'is allergic to', 'allergic_to'),
  ('predicate', 'en', 'has allergy to', 'allergic_to'),
  ('predicate', 'en', 'studies', 'studies'),
  ('predicate', 'en', 'works', 'works'),
  ('predicate', 'en', 'sleeps', 'sleeps_in'),
  ('predicate', 'en', 'cooks', 'cooks'),
  ('predicate', 'en', 'is served', 'served_at'),
  ('predicate', 'en', 'turns on', 'activates_at'),
  ('predicate', 'en', 'locks', 'closes_at'),
  ('predicate', 'en', 'closes', 'closes_at'),
  ('predicate', 'en', 'picks up the mail', 'picks_up_mail'),
  ('predicate', 'en', 'picks up mail', 'picks_up_mail'),
  ('predicate', 'en', 'opens with', 'opens_with'),
  ('predicate', 'en', 'is watered', 'watered_at'),
  ('predicate', 'en', 'charges', 'charges_at'),
  ('predicate', 'en', 'is painted', 'painted_in'),
  ('predicate', 'en', 'is set to', 'set_to'),
  ('predicate', 'en', 'beeps when', 'beeps_when'),
  ('predicate', 'en', 'eats at', 'eats_at'),
  ('predicate', 'en', 'goes to', 'goes_to'),
  ('predicate', 'en', 'is called', 'is'),
  ('predicate', 'en', 'is', 'is'),
  ('kinship', 'es', 'hermana', ''),
  ('kinship', 'es', 'hermano', ''),
  ('kinship', 'es', 'madre', ''),
  ('kinship', 'es', 'padre', ''),
  ('kinship', 'es', 'hija', ''),
  ('kinship', 'es', 'hijo', ''),
  ('kinship', 'es', 'abuela', ''),
  ('kinship', 'es', 'abuelo', ''),
  ('kinship', 'es', 'prima', ''),
  ('kinship', 'es', 'primo', ''),
  ('kinship', 'es', 'tio', ''),
  ('kinship', 'es', 'tia', ''),
  ('kinship', 'es', 'cunada', ''),
  ('kinship', 'es', 'cunado', ''),
  ('kinship', 'es', 'nieta', ''),
  ('kinship', 'es', 'nieto', ''),
  ('kinship', 'es', 'sobrina', ''),
  ('kinship', 'es', 'sobrino', ''),
  ('kinship', 'es', 'esposa', ''),
  ('kinship', 'es', 'esposo', ''),
  ('kinship', 'es', 'vecina', ''),
  ('kinship', 'es', 'vecino', ''),
  ('kinship', 'en', 'sister', ''),
  ('kinship', 'en', 'brother', ''),
  ('kinship', 'en', 'mother', ''),
  ('kinship', 'en', 'father', ''),
  ('kinship', 'en', 'daughter', ''),
  ('kinship', 'en', 'son', ''),
  ('kinship', 'en', 'grandma', ''),
  ('kinship', 'en', 'grandpa', ''),
  ('kinship', 'en', 'cousin', ''),
  ('kinship', 'en', 'uncle', ''),
  ('kinship', 'en', 'aunt', ''),
  ('kinship', 'en', 'niece', ''),
  ('kinship', 'en', 'nephew', ''),
  ('kinship', 'en', 'wife', ''),
  ('kinship', 'en', 'husband', ''),
  ('kinship', 'en', 'neighbor', ''),
  ('stopword', 'es', 'el', ''),
  ('stopword', 'es', 'la', ''),
  ('stopword', 'es', 'los', ''),
  ('stopword', 'es', 'las', ''),
  ('stopword', 'es', 'de', ''),
  ('stopword', 'es', 'en', ''),
  ('stopword', 'es', 'a', ''),
  ('stopword', 'es', 'y', ''),
  ('stopword', 'es', 'que', ''),
  ('stopword', 'es', 'mi', ''),
  ('stopword', 'es', 'su', ''),
  ('stopword', 'es', 'un', ''),
  ('stopword', 'es', 'una', ''),
  ('stopword', 'es', 'al', ''),
  ('stopword', 'es', 'del', ''),
  ('stopword', 'es', 'para', ''),
  ('stopword', 'es', 'por', ''),
  ('stopword', 'es', 'con', ''),
  ('stopword', 'es', 'sin', ''),
  ('stopword', 'es', 'se', ''),
  ('stopword', 'es', 'no', ''),
  ('stopword', 'es', 'si', ''),
  ('stopword', 'es', 'recuerda', ''),
  ('stopword', 'es', 'apunta', ''),
  ('stopword', 'es', 'oye', ''),
  ('stopword', 'es', 'hola', ''),
  ('stopword', 'es', 'argus', ''),
  ('stopword', 'en', 'the', ''),
  ('stopword', 'en', 'a', ''),
  ('stopword', 'en', 'an', ''),
  ('stopword', 'en', 'of', ''),
  ('stopword', 'en', 'in', ''),
  ('stopword', 'en', 'at', ''),
  ('stopword', 'en', 'to', ''),
  ('stopword', 'en', 'and', ''),
  ('stopword', 'en', 'that', ''),
  ('stopword', 'en', 'my', ''),
  ('stopword', 'en', 'his', ''),
  ('stopword', 'en', 'her', ''),
  ('stopword', 'en', 'our', ''),
  ('stopword', 'en', 'your', ''),
  ('stopword', 'en', 'for', ''),
  ('stopword', 'en', 'with', ''),
  ('stopword', 'en', 'without', ''),
  ('stopword', 'en', 'no', ''),
  ('stopword', 'en', 'not', ''),
  ('stopword', 'en', 'on', ''),
  ('stopword', 'en', 'remember', ''),
  ('stopword', 'en', 'note', ''),
  ('stopword', 'en', 'hey', ''),
  ('stopword', 'en', 'hello', ''),
  ('stopword', 'en', 'argus', '');

INSERT OR IGNORE INTO memory_lexicon (kind, lang, surface, canonical) VALUES
  ('first_person', 'es', 'me', ''),
  ('first_person', 'es', 'mi', ''),
  ('first_person', 'es', 'yo', ''),
  ('first_person', 'es', 'nosotros', ''),
  ('first_person', 'en', 'i', ''),
  ('first_person', 'en', 'me', ''),
  ('first_person', 'en', 'my', ''),
  ('first_person', 'en', 'we', ''),
  ('first_person', 'en', 'our', '');

INSERT OR IGNORE INTO memory_phrase (kind, lang, phrase, memory_type) VALUES
  ('trigger', 'es', 'me recuerdas que', 'persona'),
  ('trigger', 'es', 'me recuerdas acerca de que', 'persona'),
  ('trigger', 'es', 'quiero que me recuerdes que', 'persona'),
  ('trigger', 'es', 'quiero que me recuerdes acerca de que', 'persona'),
  ('trigger', 'es', 'quiero que me recuerdas que', 'persona'),
  ('trigger', 'es', 'quiero que me recuerdas acerca de que', 'persona'),
  ('trigger', 'es', 'recuerdame que', 'persona'),
  ('trigger', 'es', 'recuerdame acerca de que', 'persona'),
  ('trigger', 'es', 'recuérdame acerca de que', 'persona'),
  ('trigger', 'es', 'apunta acerca de que', 'persona'),
  ('trigger', 'en', 'remind me about that', 'persona'),
  ('trigger', 'en', 'i want you to remind me that', 'persona');

INSERT OR IGNORE INTO memory_lexicon (kind, lang, surface, canonical) VALUES
  ('predicate', 'es', 'está en', 'located_at'),
  ('predicate', 'es', 'esta en', 'located_at'),
  ('predicate', 'es', 'está', 'located_at'),
  ('predicate', 'es', 'esta', 'located_at'),
  ('predicate', 'es', 'queda en', 'located_at'),
  ('predicate', 'es', 'vive en', 'lives_in'),
  ('predicate', 'es', 'se guarda en', 'stored_in'),
  ('predicate', 'es', 'se guardan en', 'stored_in'),
  ('predicate', 'es', 'está guardado en', 'stored_in'),
  ('predicate', 'en', 'is in', 'located_at'),
  ('predicate', 'en', 'is at', 'located_at'),
  ('predicate', 'en', 'lives in', 'lives_in'),
  ('predicate', 'en', 'is kept in', 'stored_in'),
  ('predicate', 'en', 'is stored in', 'stored_in');

INSERT OR IGNORE INTO memory_phrase (kind, lang, phrase, memory_type) VALUES
  ('interrogative', 'es', 'que', 'persona'),
  ('interrogative', 'es', 'qué', 'persona'),
  ('interrogative', 'es', 'cuando', 'persona'),
  ('interrogative', 'es', 'cuándo', 'persona'),
  ('interrogative', 'es', 'donde', 'persona'),
  ('interrogative', 'es', 'dónde', 'persona'),
  ('interrogative', 'es', 'quien', 'persona'),
  ('interrogative', 'es', 'quién', 'persona'),
  ('interrogative', 'es', 'como', 'persona'),
  ('interrogative', 'es', 'cómo', 'persona'),
  ('interrogative', 'es', 'cual', 'persona'),
  ('interrogative', 'es', 'cuál', 'persona'),
  ('interrogative', 'es', 'cuanto', 'persona'),
  ('interrogative', 'es', 'cuánto', 'persona'),
  ('interrogative', 'es', 'cuanta', 'persona'),
  ('interrogative', 'es', 'cuánta', 'persona'),
  ('interrogative', 'es', 'por que', 'persona'),
  ('interrogative', 'es', 'por qué', 'persona'),
  ('interrogative', 'es', 'a que', 'persona'),
  ('interrogative', 'es', 'a qué', 'persona'),
  ('interrogative', 'en', 'what', 'persona'),
  ('interrogative', 'en', 'when', 'persona'),
  ('interrogative', 'en', 'where', 'persona'),
  ('interrogative', 'en', 'who', 'persona'),
  ('interrogative', 'en', 'how', 'persona'),
  ('interrogative', 'en', 'which', 'persona'),
  ('interrogative', 'en', 'why', 'persona'),
  ('recall_marker', 'es', 'queria saber', 'persona'),
  ('recall_marker', 'es', 'quería saber', 'persona'),
  ('recall_marker', 'es', 'queria preguntar', 'persona'),
  ('recall_marker', 'es', 'quería preguntar', 'persona'),
  ('recall_marker', 'es', 'me preguntaba', 'persona'),
  ('recall_marker', 'es', 'te quería preguntar', 'persona'),
  ('recall_marker', 'es', 'te queria preguntar', 'persona');

INSERT OR IGNORE INTO memory_phrase (kind, lang, phrase, memory_type) VALUES
  ('interrogative', 'es', 'que', 'persona'),
  ('interrogative', 'es', 'qué', 'persona'),
  ('interrogative', 'es', 'cuando', 'persona'),
  ('interrogative', 'es', 'cuándo', 'persona'),
  ('interrogative', 'es', 'donde', 'persona'),
  ('interrogative', 'es', 'dónde', 'persona'),
  ('interrogative', 'es', 'quien', 'persona'),
  ('interrogative', 'es', 'quién', 'persona'),
  ('interrogative', 'es', 'como', 'persona'),
  ('interrogative', 'es', 'cómo', 'persona'),
  ('interrogative', 'es', 'cual', 'persona'),
  ('interrogative', 'es', 'cuál', 'persona'),
  ('interrogative', 'es', 'cuanto', 'persona'),
  ('interrogative', 'es', 'cuánto', 'persona'),
  ('interrogative', 'es', 'por que', 'persona'),
  ('interrogative', 'es', 'por qué', 'persona'),
  ('interrogative', 'es', 'a que', 'persona'),
  ('interrogative', 'es', 'a qué', 'persona'),
  ('interrogative', 'en', 'what', 'persona'),
  ('interrogative', 'en', 'when', 'persona'),
  ('interrogative', 'en', 'where', 'persona'),
  ('interrogative', 'en', 'who', 'persona'),
  ('interrogative', 'en', 'how', 'persona'),
  ('interrogative', 'en', 'which', 'persona'),
  ('interrogative', 'en', 'why', 'persona'),
  ('recall_marker', 'es', 'queria saber', 'persona'),
  ('recall_marker', 'es', 'quería saber', 'persona'),
  ('recall_marker', 'es', 'queria preguntar', 'persona'),
  ('recall_marker', 'es', 'quería preguntar', 'persona'),
  ('recall_marker', 'es', 'me preguntaba', 'persona');

INSERT OR IGNORE INTO memory_phrase (kind, lang, phrase, memory_type) VALUES
  ('filler', 'es', 'oye', 'persona'),
  ('filler', 'es', 'mira', 'persona'),
  ('filler', 'es', 'mirá', 'persona'),
  ('filler', 'es', 'este', 'persona'),
  ('filler', 'es', 'esto', 'persona'),
  ('filler', 'es', 'bueno', 'persona'),
  ('filler', 'es', 'pues', 'persona'),
  ('filler', 'es', 'a ver', 'persona'),
  ('filler', 'es', 'eh', 'persona'),
  ('filler', 'es', 'ehm', 'persona'),
  ('filler', 'es', 'em', 'persona'),
  ('filler', 'es', 'o sea', 'persona'),
  ('filler', 'es', 'digamos', 'persona'),
  ('filler', 'es', 'sabes', 'persona'),
  ('filler', 'es', 'vale', 'persona'),
  ('filler', 'es', 'nada', 'persona'),
  ('filler', 'es', 'en fin', 'persona'),
  ('filler', 'es', 'pues nada', 'persona'),
  ('filler', 'es', 'hola', 'persona'),
  ('filler', 'es', 'argus', 'persona'),
  ('filler', 'es', 'argos', 'persona'),
  ('filler', 'es', 'largus', 'persona'),
  ('filler', 'en', 'hey', 'persona'),
  ('filler', 'en', 'look', 'persona'),
  ('filler', 'en', 'well', 'persona'),
  ('filler', 'en', 'so', 'persona'),
  ('filler', 'en', 'um', 'persona'),
  ('filler', 'en', 'uh', 'persona'),
  ('filler', 'en', 'you know', 'persona'),
  ('filler', 'en', 'okay', 'persona'),
  ('filler', 'en', 'ok', 'persona'),
  ('trigger', 'es', 'apuntalo', 'persona'),
  ('trigger', 'es', 'apúntalo', 'persona'),
  ('trigger', 'es', 'anotalo', 'persona'),
  ('trigger', 'es', 'anótalo', 'persona'),
  ('trigger', 'es', 'recuerdalo', 'persona'),
  ('trigger', 'es', 'recuérdalo', 'persona'),
  ('trigger', 'es', 'no lo olvides', 'persona'),
  ('trigger', 'es', 'tenlo en cuenta', 'persona'),
  ('trigger', 'es', 'apunta esto', 'persona'),
  ('trigger', 'es', 'anota esto', 'persona');

INSERT OR IGNORE INTO memory_phrase (kind, lang, phrase, memory_type) VALUES
  ('filler', 'es', 'una cosa', 'persona'),
  ('filler', 'es', 'otra cosa', 'persona'),
  ('filler', 'es', 'una cosita', 'persona'),
  ('filler', 'es', 'a proposito', 'persona'),
  ('filler', 'es', 'por cierto', 'persona'),
  ('filler', 'es', 'escucha', 'persona'),
  ('filler', 'es', 'perdon', 'persona'),
  ('filler', 'es', 'perdona', 'persona');

INSERT OR IGNORE INTO memory_phrase (kind, lang, phrase, memory_type) VALUES
  ('filler', 'es', 'es que', 'persona'),
  ('filler', 'es', 'lo que pasa es que', 'persona'),
  ('filler', 'es', 'resulta que', 'persona'),
  ('filler', 'es', 'que', 'persona'),
  ('filler', 'es', 'y', 'persona'),
  ('filler', 'es', 'pero', 'persona'),
  ('filler', 'es', 'o sea que', 'persona'),
  ('filler', 'es', 'asi que', 'persona');
