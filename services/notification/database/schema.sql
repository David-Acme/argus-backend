PRAGMA journal_mode       = WAL;
PRAGMA synchronous        = NORMAL;
PRAGMA busy_timeout       = 5000;
PRAGMA cache_size         = -64000;
PRAGMA temp_store         = MEMORY;
PRAGMA mmap_size          = 268435456;
PRAGMA foreign_keys       = ON;
PRAGMA journal_size_limit = 67108864;

CREATE TABLE IF NOT EXISTS notification (
    id         INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id    INTEGER NOT NULL,
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
    user_id     INTEGER NOT NULL,
    device_hash TEXT    NOT NULL  DEFAULT '',
    token       TEXT    NOT NULL  UNIQUE,
    platform    TEXT    NOT NULL  DEFAULT '',
    lang        TEXT    NOT NULL  DEFAULT '',
    is_active   INTEGER NOT NULL  DEFAULT 1  CHECK (is_active IN (0, 1)),
    created_at  INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at  INTEGER,
    session_id  TEXT    NOT NULL  DEFAULT ''
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
    acked_ms        INTEGER NOT NULL  DEFAULT 0,
    claimed_at      INTEGER NOT NULL  DEFAULT 0
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
DROP INDEX IF EXISTS idx_notification_token_user;

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

CREATE TABLE IF NOT EXISTS camera_fallback_event (
    id         INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    camera_id  INTEGER NOT NULL  DEFAULT 0,
    rule       TEXT    NOT NULL  DEFAULT '',
    severity   TEXT    NOT NULL  DEFAULT '',
    reason     TEXT    NOT NULL  DEFAULT ''
                         CHECK (reason IN ('non_hard_signal', 'drop_known',
                                'drop_weak_score', 'drop_short_dwell',
                                'budget_silent')),
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_camera_fallback_created
    ON camera_fallback_event (created_at DESC);

CREATE TABLE IF NOT EXISTS call_preference (
    user_id            INTEGER NOT NULL  PRIMARY KEY,
    enabled            INTEGER NOT NULL  DEFAULT 1  CHECK (enabled IN (0, 1)),
    guard_critical     TEXT    NOT NULL  DEFAULT 'call'
                                  CHECK (guard_critical IN ('call', 'notify', 'off')),
    guard_intruder     TEXT    NOT NULL  DEFAULT 'call'
                                  CHECK (guard_intruder IN ('call', 'notify', 'off')),
    guard_escalation   TEXT    NOT NULL  DEFAULT 'call'
                                  CHECK (guard_escalation IN ('call', 'notify', 'off')),
    guard_arrival      TEXT    NOT NULL  DEFAULT 'off'
                                  CHECK (guard_arrival IN ('call', 'notify', 'off')),
    agenda             TEXT    NOT NULL  DEFAULT 'call'
                                  CHECK (agenda IN ('call', 'notify', 'off')),
    assistant          TEXT    NOT NULL  DEFAULT 'call'
                                  CHECK (assistant IN ('call', 'notify', 'off')),
    quiet_start_hour   INTEGER NOT NULL  DEFAULT -1,
    quiet_end_hour     INTEGER NOT NULL  DEFAULT -1,
    dnd_until          INTEGER NOT NULL  DEFAULT 0,
    critical_bypass    INTEGER NOT NULL  DEFAULT 1  CHECK (critical_bypass IN (0, 1)),
    muted_environments TEXT    NOT NULL  DEFAULT '[]',
    updated_at         INTEGER NOT NULL  DEFAULT 0,
    agenda_lead_min    INTEGER NOT NULL  DEFAULT 10,
    quiet_days         INTEGER NOT NULL  DEFAULT 127,
    ring_seconds       INTEGER NOT NULL  DEFAULT 45,
    push_delay_s       INTEGER NOT NULL  DEFAULT 4,
    live_announce      INTEGER NOT NULL  DEFAULT 1  CHECK (live_announce IN (0, 1)),
    lang               TEXT    NOT NULL  DEFAULT ''  CHECK (lang IN ('', 'es', 'en'))
);

CREATE TABLE IF NOT EXISTS call (
    id               INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id          INTEGER NOT NULL,
    dedupe_key       TEXT    NOT NULL,
    trigger          TEXT    NOT NULL
                             CHECK (trigger IN ('guard_critical', 'guard_intruder',
                                    'guard_escalation', 'guard_arrival',
                                    'agenda', 'assistant')),
    state            TEXT    NOT NULL
                             CHECK (state IN ('ringing', 'queued', 'answered',
                                    'completed', 'missed', 'declined',
                                    'injected')),
    reason           TEXT    NOT NULL  DEFAULT '',
    parent_call_id   INTEGER NOT NULL  DEFAULT 0,
    urgency          TEXT    NOT NULL  DEFAULT 'active',
    lang             TEXT    NOT NULL  DEFAULT 'es',
    title            TEXT    NOT NULL  DEFAULT '',
    summary          TEXT    NOT NULL  DEFAULT '',
    opening_line     TEXT    NOT NULL  DEFAULT '',
    missed_line      TEXT    NOT NULL  DEFAULT '',
    data             TEXT    NOT NULL  DEFAULT '{}',
    answered_session TEXT    NOT NULL  DEFAULT '',
    created_at       INTEGER NOT NULL  DEFAULT 0,
    expires_at       INTEGER NOT NULL  DEFAULT 0,
    pushed_at        INTEGER NOT NULL  DEFAULT 0,
    answered_at      INTEGER NOT NULL  DEFAULT 0,
    ended_at         INTEGER NOT NULL  DEFAULT 0,
    push_after       INTEGER NOT NULL  DEFAULT 0
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_call_dedupe ON call (dedupe_key, user_id);
CREATE INDEX IF NOT EXISTS idx_call_user_created ON call (user_id, created_at);
CREATE INDEX IF NOT EXISTS idx_call_live ON call (state, expires_at)
    WHERE state IN ('ringing', 'queued', 'answered');

CREATE TABLE IF NOT EXISTS scheduled_call (
    id          INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id     INTEGER NOT NULL,
    command_id  TEXT    NOT NULL  UNIQUE,
    fire_at     INTEGER NOT NULL,
    topic       TEXT    NOT NULL,
    lang        TEXT    NOT NULL  DEFAULT 'es',
    state       TEXT    NOT NULL  DEFAULT 'pending'
                        CHECK (state IN ('pending', 'fired', 'canceled')),
    created_at  INTEGER NOT NULL  DEFAULT 0,
    fired_at    INTEGER NOT NULL  DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_scheduled_call_due ON scheduled_call (state, fire_at);

CREATE TABLE IF NOT EXISTS call_arrival_seen (
    person_id  INTEGER NOT NULL  PRIMARY KEY,
    last_seen  INTEGER NOT NULL  DEFAULT 0
);

CREATE TABLE IF NOT EXISTS call_response (
    id              INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    dedupe_key      TEXT    NOT NULL UNIQUE,
    kind            TEXT    NOT NULL
                            CHECK (kind IN ('guard_episode', 'guard_panic',
                                   'guard_duress', 'guard_tamper')),
    environment_id  INTEGER NOT NULL DEFAULT 0,
    episode_id      INTEGER NOT NULL DEFAULT 0,
    camera_id       INTEGER NOT NULL DEFAULT 0,
    strategy        TEXT    NOT NULL DEFAULT 'ordered'
                            CHECK (strategy IN ('ordered', 'inside_first',
                                   'everyone', 'night_quiet')),
    state           TEXT    NOT NULL DEFAULT 'active'
                            CHECK (state IN ('active', 'attended', 'unanswered',
                                   'confirmed', 'false_alarm', 'expired')),
    step            INTEGER NOT NULL DEFAULT 0,
    step_count      INTEGER NOT NULL DEFAULT 1,
    step_seconds    INTEGER NOT NULL DEFAULT 45,
    step_deadline   INTEGER NOT NULL DEFAULT 0,
    responder_id    INTEGER NOT NULL DEFAULT 0,
    responder_name  TEXT    NOT NULL DEFAULT '',
    verdict         TEXT    NOT NULL DEFAULT ''
                            CHECK (verdict IN ('', 'real', 'false_alarm')),
    verdict_by      INTEGER NOT NULL DEFAULT 0,
    verdict_by_name TEXT    NOT NULL DEFAULT '',
    verdict_at      INTEGER NOT NULL DEFAULT 0,
    plan            TEXT    NOT NULL DEFAULT '{}',
    data            TEXT    NOT NULL DEFAULT '{}',
    created_at      INTEGER NOT NULL DEFAULT 0,
    updated_at      INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_call_response_due
    ON call_response (state, step_deadline);

CREATE TABLE IF NOT EXISTS call_response_member (
    response_id INTEGER NOT NULL,
    user_id     INTEGER NOT NULL,
    step        INTEGER NOT NULL DEFAULT 0,
    mode        TEXT    NOT NULL DEFAULT 'call' CHECK (mode IN ('call', 'notify')),
    mandatory   INTEGER NOT NULL DEFAULT 0 CHECK (mandatory IN (0, 1)),
    discreet    INTEGER NOT NULL DEFAULT 0 CHECK (discreet IN (0, 1)),
    reached_at  INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (response_id, user_id)
) WITHOUT ROWID;

CREATE INDEX IF NOT EXISTS idx_call_response_member_user
    ON call_response_member (user_id, response_id);
