-- ─────────────────────────────────────────────────────────────────────────────
-- Argus productivity  ·  Productivity schema (productivity.db)
-- The 7 productivity tables plus their 13 indexes, copied verbatim from
-- database/schema.sql (source of truth): reminder, project, project_task,
-- calendar_event, project_member, calendar_event_share, reminder_detail.
-- context_note stays a frozen argus.db orphan and is NOT recreated here
-- (Ruling AK; the drop decision is the user's). Applied by
-- tools/migrate-productivity and by argus-productivity at boot. argus.db is
-- never touched.
-- Structure: pragmas → table creation → indexes (inline, verbatim order).
-- ─────────────────────────────────────────────────────────────────────────────

PRAGMA journal_mode       = WAL;
PRAGMA synchronous        = NORMAL;
PRAGMA busy_timeout       = 5000;
PRAGMA cache_size         = -64000;
PRAGMA temp_store         = MEMORY;
PRAGMA mmap_size          = 268435456;
-- The user rows live in identity.db, not here: foreign keys stay off and the
-- share/member targets are validated in code against the identity client
-- (Ruling AM). The REFERENCES clauses below are kept verbatim from
-- schema.sql.
PRAGMA foreign_keys       = OFF;
PRAGMA journal_size_limit = 67108864;

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

CREATE INDEX IF NOT EXISTS idx_reminder_target_user  ON reminder (target_user_id);
CREATE INDEX IF NOT EXISTS idx_reminder_scheduled    ON reminder (scheduled_at);
CREATE INDEX IF NOT EXISTS idx_reminder_created_at   ON reminder (created_at);
CREATE INDEX IF NOT EXISTS idx_reminder_deleted_at   ON reminder (deleted_at);

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

CREATE INDEX IF NOT EXISTS idx_reminder_detail_reminder  ON reminder_detail (reminder_id);
CREATE INDEX IF NOT EXISTS idx_reminder_detail_created   ON reminder_detail (created_at);
CREATE INDEX IF NOT EXISTS idx_reminder_detail_deleted   ON reminder_detail (deleted_at);
