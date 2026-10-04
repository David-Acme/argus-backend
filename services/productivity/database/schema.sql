PRAGMA journal_mode       = WAL;
PRAGMA synchronous        = NORMAL;
PRAGMA busy_timeout       = 5000;
PRAGMA cache_size         = -64000;
PRAGMA temp_store         = MEMORY;
PRAGMA mmap_size          = 268435456;
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
    ends_at         INTEGER,
    is_all_day      INTEGER NOT NULL  DEFAULT 0  CHECK (is_all_day IN (0, 1)),
    recurrence_rule TEXT,
    created_at      INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at      INTEGER,
    deleted_at      INTEGER
);

CREATE INDEX IF NOT EXISTS idx_calendar_event_owner_start
    ON calendar_event(owner_id, starts_at);

CREATE TABLE IF NOT EXISTS project_member (
    id         INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER NOT NULL  REFERENCES project(id) ON DELETE CASCADE,
    user_id    INTEGER NOT NULL  REFERENCES user(id)    ON DELETE CASCADE,
    access     TEXT    NOT NULL  DEFAULT 'view'  CHECK (access IN ('view', 'edit')),
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at INTEGER,
    deleted_at INTEGER
);

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

CREATE INDEX IF NOT EXISTS idx_project_live_created
    ON project (created_at, id) WHERE deleted_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_project_deleted_at
    ON project (deleted_at, id) WHERE deleted_at IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_project_task_live_created
    ON project_task (created_at, id) WHERE deleted_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_project_task_deleted_at
    ON project_task (deleted_at, id) WHERE deleted_at IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_calendar_event_live_created
    ON calendar_event (created_at, id) WHERE deleted_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_calendar_event_deleted_at
    ON calendar_event (deleted_at, id) WHERE deleted_at IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_project_member_live_created
    ON project_member (created_at, id) WHERE deleted_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_project_member_deleted_at
    ON project_member (deleted_at, id) WHERE deleted_at IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_calendar_event_share_live_created
    ON calendar_event_share (created_at, id) WHERE deleted_at IS NULL;
CREATE INDEX IF NOT EXISTS idx_calendar_event_share_deleted_at
    ON calendar_event_share (deleted_at, id) WHERE deleted_at IS NOT NULL;

CREATE TABLE IF NOT EXISTS idempotency_key (
    user_id    INTEGER NOT NULL,
    idem_key   TEXT    NOT NULL,
    route      TEXT    NOT NULL,
    record_id  INTEGER NOT NULL,
    created_at INTEGER NOT NULL,
    PRIMARY KEY (user_id, idem_key)
);

CREATE INDEX IF NOT EXISTS idx_idempotency_key_created
    ON idempotency_key (created_at);

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

CREATE INDEX IF NOT EXISTS idx_calendar_event_live_start
    ON calendar_event (starts_at) WHERE deleted_at IS NULL AND is_all_day = 0;

DROP TABLE IF EXISTS agenda_announcement;

CREATE TABLE IF NOT EXISTS agenda_notice (
    kind          TEXT    NOT NULL  CHECK (kind IN ('event', 'reminder')),
    ref_id        INTEGER NOT NULL,
    occurrence_at INTEGER NOT NULL,
    lead_minutes  INTEGER NOT NULL  DEFAULT 0,
    created_at    INTEGER NOT NULL  DEFAULT 0,
    PRIMARY KEY (kind, ref_id, occurrence_at, lead_minutes)
);
