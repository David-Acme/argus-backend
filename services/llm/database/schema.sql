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

CREATE TABLE IF NOT EXISTS catalog_person (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id    INTEGER,
  name       TEXT NOT NULL DEFAULT '',
  alias      TEXT NOT NULL DEFAULT '',
  deleted_at INTEGER
);

CREATE TABLE IF NOT EXISTS catalog_camera (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  name       TEXT NOT NULL DEFAULT '',
  deleted_at INTEGER
);

CREATE TABLE IF NOT EXISTS catalog_zone (
  id   INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS catalog_stream (
  id    INTEGER PRIMARY KEY AUTOINCREMENT,
  label TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS encounter_closed_inbox (
  event_id        TEXT    NOT NULL  PRIMARY KEY,
  fingerprint     TEXT    NOT NULL  DEFAULT '',
  attempts        INTEGER NOT NULL  DEFAULT 0,
  status          TEXT    NOT NULL  DEFAULT 'received'
                  CHECK (status IN ('received', 'dispatched', 'conflict',
                                    'dead_lettered')),
  created_at      INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
  updated_at      INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_encounter_closed_inbox_settled
  ON encounter_closed_inbox (updated_at) WHERE status != 'received';

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

CREATE INDEX IF NOT EXISTS idx_encounter_closed_inbox_status
    ON encounter_closed_inbox (status, event_id);
