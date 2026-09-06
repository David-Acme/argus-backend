-- ─────────────────────────────────────────────────────────────────────────────
-- Argus notification  ·  Notification schema (notification.db)
-- The 2 notification tables, copied verbatim from database/schema.sql (source
-- of truth): notification, notification_token, plus the 3 indexes schema.sql
-- defines — the unique notification_token one backs the ON CONFLICT target of
-- the token upsert, so without it the legacy registerToken statement fails to
-- prepare. Applied by tools/migrate-notification and by argus-notification at
-- boot. argus.db is never touched.
-- Structure: pragmas → table creation → indexes (inline, verbatim order).
-- ─────────────────────────────────────────────────────────────────────────────

PRAGMA journal_mode       = WAL;
PRAGMA synchronous        = NORMAL;
PRAGMA busy_timeout       = 5000;
PRAGMA cache_size         = -64000;
PRAGMA temp_store         = MEMORY;
PRAGMA mmap_size          = 268435456;
-- The user rows live in identity.db, not here: foreign keys stay off and the
-- recipient user is validated in code (JWT context). The REFERENCES clauses
-- below are kept verbatim from schema.sql.
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
CREATE INDEX IF NOT EXISTS idx_notification_token_user ON notification_token (user_id);
