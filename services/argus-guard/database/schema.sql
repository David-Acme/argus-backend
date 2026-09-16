-- ─────────────────────────────────────────────────────────────────────────────
-- Argus guard  ·  Guard schema (guard.db)
-- Autonomous camera security: incidents, executed actions and runtime state.
-- Applied by argus-guard at boot. argus.db is never touched.
-- Structure: pragmas → table creation → indexes.
-- ─────────────────────────────────────────────────────────────────────────────

PRAGMA journal_mode       = WAL;
PRAGMA synchronous        = NORMAL;
PRAGMA busy_timeout       = 5000;
PRAGMA cache_size         = -64000;
PRAGMA temp_store         = MEMORY;
PRAGMA mmap_size          = 268435456;
PRAGMA foreign_keys       = OFF;
PRAGMA journal_size_limit = 67108864;

CREATE TABLE IF NOT EXISTS guard_incident (
    id          INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    camera_id   INTEGER NOT NULL,
    camera_name TEXT    NOT NULL  DEFAULT '',
    rule        TEXT    NOT NULL  DEFAULT '',
    danger      TEXT    NOT NULL  DEFAULT 'none'
                        CHECK (danger IN ('none', 'low', 'medium', 'high', 'critical')),
    severity    TEXT    NOT NULL  DEFAULT '',
    person_id   INTEGER NOT NULL  DEFAULT 0,
    identity    TEXT    NOT NULL  DEFAULT '',
    event_id    TEXT    NOT NULL  DEFAULT '',
    event_json  TEXT    NOT NULL  DEFAULT '{}',
    created_at  INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_guard_incident_camera_created
    ON guard_incident (camera_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_guard_incident_person_created
    ON guard_incident (person_id, created_at DESC);
CREATE UNIQUE INDEX IF NOT EXISTS idx_guard_incident_event
    ON guard_incident (event_id) WHERE event_id != '';

CREATE TABLE IF NOT EXISTS guard_action (
    id          INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    incident_id INTEGER NOT NULL  DEFAULT 0,
    encounter_id INTEGER NOT NULL DEFAULT 0,
    camera_id   INTEGER NOT NULL  DEFAULT 0,
    person_id   INTEGER NOT NULL  DEFAULT 0,
    command_id  TEXT    NOT NULL  DEFAULT '',
    kind        TEXT    NOT NULL  DEFAULT ''
                  CHECK (kind IN ('greet', 'greet_listen', 'greet_reply',
                         'announce', 'alarm', 'siren_arm', 'siren_disarm',
                         'notify') OR kind LIKE 'agent\_%' ESCAPE '\'),
    status      TEXT    NOT NULL  DEFAULT '',
    detail      TEXT    NOT NULL  DEFAULT '',
    created_at  INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_guard_action_camera_created
    ON guard_action (camera_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_guard_action_encounter_created
    ON guard_action (encounter_id, created_at DESC);
CREATE UNIQUE INDEX IF NOT EXISTS idx_guard_action_command
    ON guard_action (command_id) WHERE command_id != '';

CREATE TABLE IF NOT EXISTS guard_state (
    key        TEXT    NOT NULL  PRIMARY KEY,
    value      TEXT    NOT NULL  DEFAULT '',
    updated_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS guard_expected_guest (
    id           INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    description  TEXT    NOT NULL  DEFAULT '',
    camera_id    INTEGER NOT NULL  DEFAULT 0,
    person_id    INTEGER NOT NULL  DEFAULT 0,
    host_user_id INTEGER NOT NULL  DEFAULT 0,
    one_time     INTEGER NOT NULL  DEFAULT 0  CHECK (one_time IN (0, 1)),
    valid_from   INTEGER NOT NULL,
    valid_until  INTEGER NOT NULL,
    used_at      INTEGER NOT NULL  DEFAULT 0,
    created_at   INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    deleted_at   INTEGER
);

CREATE INDEX IF NOT EXISTS idx_guard_expected_guest_window
    ON guard_expected_guest (valid_from, valid_until) WHERE deleted_at IS NULL;

CREATE TABLE IF NOT EXISTS guard_assessment (
    id          INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    incident_id INTEGER NOT NULL  DEFAULT 0,
    camera_id   INTEGER NOT NULL  DEFAULT 0,
    event_id    TEXT    NOT NULL  DEFAULT '',
    mode        TEXT    NOT NULL  DEFAULT '',
    caption     TEXT    NOT NULL  DEFAULT '',
    threat      TEXT    NOT NULL  DEFAULT '',
    veto        INTEGER NOT NULL  DEFAULT 0  CHECK (veto IN (0, 1)),
    tags        TEXT    NOT NULL  DEFAULT '[]',
    summary     TEXT    NOT NULL  DEFAULT '',
    created_at  INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_guard_assessment_incident
    ON guard_assessment (incident_id);
CREATE UNIQUE INDEX IF NOT EXISTS idx_guard_assessment_event
    ON guard_assessment (event_id) WHERE event_id != '';

CREATE TABLE IF NOT EXISTS guard_encounter (
    id             INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    person_id      INTEGER NOT NULL  DEFAULT 0,
    signature      TEXT    NOT NULL  DEFAULT '',
    state          TEXT    NOT NULL  DEFAULT 'observing'
                           CHECK (state IN ('observing', 'verifying', 'challenging',
                                            'listening', 'interpreting', 'resolved',
                                            'escalating', 'degraded', 'closed')),
    grade          TEXT    NOT NULL  DEFAULT 'none',
    checks         INTEGER NOT NULL  DEFAULT 0,
    revision       INTEGER NOT NULL  DEFAULT 0,
    best_camera_id INTEGER NOT NULL  DEFAULT 0,
    best_score     REAL    NOT NULL  DEFAULT 0,
    dialogue_turns   INTEGER NOT NULL  DEFAULT 0,
    dialogue_turn_key TEXT   NOT NULL  DEFAULT '',
    dialogue_goal    TEXT    NOT NULL  DEFAULT '',
    listening_until  INTEGER NOT NULL  DEFAULT 0,
    last_line        TEXT    NOT NULL  DEFAULT '',
    last_heard       TEXT    NOT NULL  DEFAULT '',
    last_heard_at    INTEGER NOT NULL  DEFAULT 0,
    first_seen     INTEGER NOT NULL,
    last_seen      INTEGER NOT NULL,
    last_action_at INTEGER NOT NULL  DEFAULT 0,
    notify_command_id TEXT NOT NULL DEFAULT '',
    notify_count      INTEGER NOT NULL DEFAULT 0,
    notify_highest_rank INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_guard_encounter_last_seen
    ON guard_encounter (last_seen DESC);
CREATE INDEX IF NOT EXISTS idx_guard_encounter_person
    ON guard_encounter (person_id);

CREATE TABLE IF NOT EXISTS guard_encounter_transition (
    id           INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    encounter_id INTEGER NOT NULL,
    from_state   TEXT    NOT NULL  DEFAULT '',
    to_state     TEXT    NOT NULL,
    reason       TEXT    NOT NULL  DEFAULT '',
    revision     INTEGER NOT NULL  DEFAULT 0,
    occurred_at  INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_guard_transition_encounter
    ON guard_encounter_transition (encounter_id, occurred_at DESC);

CREATE TABLE IF NOT EXISTS guard_observation_inbox (
    event_id       TEXT    NOT NULL  PRIMARY KEY,
    camera_id      INTEGER NOT NULL  DEFAULT 0,
    observation_id TEXT    NOT NULL  DEFAULT '',
    status         TEXT    NOT NULL  DEFAULT 'processing'
                           CHECK (status IN ('processing', 'completed', 'dead_lettered')),
    attempts       INTEGER NOT NULL  DEFAULT 0,
    stage          INTEGER NOT NULL  DEFAULT 0,
    incident_id    INTEGER NOT NULL  DEFAULT 0,
    encounter_id   INTEGER NOT NULL  DEFAULT 0,
    danger         TEXT    NOT NULL  DEFAULT '',
    checkpoint     TEXT    NOT NULL  DEFAULT '{}',
    payload        TEXT    NOT NULL  DEFAULT '',
    retry_at       INTEGER NOT NULL  DEFAULT 0,
    local_retries  INTEGER NOT NULL  DEFAULT 0,
    received_at    INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at     INTEGER NOT NULL  DEFAULT 0,
    completed_at   INTEGER NOT NULL  DEFAULT 0
);

CREATE TABLE IF NOT EXISTS guard_dead_letter (
    id         INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    event_id   TEXT    NOT NULL  DEFAULT '',
    payload    TEXT    NOT NULL  DEFAULT '',
    reason     TEXT    NOT NULL  DEFAULT '',
    attempts   INTEGER NOT NULL  DEFAULT 0,
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_guard_dead_letter_event
    ON guard_dead_letter (event_id) WHERE event_id != '';

CREATE TABLE IF NOT EXISTS guard_action_outbox (
    command_id   TEXT    NOT NULL  PRIMARY KEY,
    encounter_id INTEGER NOT NULL  DEFAULT 0,
    incident_id  INTEGER NOT NULL  DEFAULT 0,
    camera_id    INTEGER NOT NULL  DEFAULT 0,
    person_id    INTEGER NOT NULL  DEFAULT 0,
    kind         TEXT    NOT NULL  DEFAULT '',
    status       TEXT    NOT NULL  DEFAULT 'pending'
        CHECK (status IN ('pending', 'in_flight', 'retryable_failed',
                          'succeeded', 'duplicate_succeeded', 'rejected',
                          'conflict', 'indeterminate')),
    detail       TEXT    NOT NULL  DEFAULT '',
    response     TEXT    NOT NULL  DEFAULT '{}',
    payload      TEXT    NOT NULL  DEFAULT '',
    attempts     INTEGER NOT NULL  DEFAULT 0,
    next_attempt_at INTEGER NOT NULL  DEFAULT 0,
    created_at   INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at   INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_guard_outbox_status
    ON guard_action_outbox (status, created_at DESC);

-- Durable encounter_closed fan-out: enqueued as the encounter closes, then
-- published with PubAck by the sweeper.
CREATE TABLE IF NOT EXISTS guard_encounter_outbox (
    event_id   TEXT    NOT NULL PRIMARY KEY,
    payload    TEXT    NOT NULL DEFAULT '{}',
    status     TEXT    NOT NULL DEFAULT 'pending',
    attempts   INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    updated_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_guard_encounter_outbox_status
    ON guard_encounter_outbox (status, created_at ASC);

-- Private evidence objects: one row per uploaded incident record.
CREATE TABLE IF NOT EXISTS guard_evidence (
    id              INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    incident_id     INTEGER NOT NULL  DEFAULT 0,
    encounter_id    INTEGER NOT NULL  DEFAULT 0,
    camera_id       INTEGER NOT NULL  DEFAULT 0,
    object_key      TEXT    NOT NULL  DEFAULT '',
    content_type    TEXT    NOT NULL  DEFAULT '',
    retention_class TEXT    NOT NULL  DEFAULT 'standard',
    created_at      INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    expires_at      INTEGER NOT NULL  DEFAULT 0,
    deleted_at      INTEGER NOT NULL  DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_guard_evidence_expires
    ON guard_evidence (expires_at) WHERE deleted_at = 0;
CREATE UNIQUE INDEX IF NOT EXISTS idx_guard_evidence_object
    ON guard_evidence (object_key) WHERE object_key != '';

-- Per-camera, per-hour-of-week decayed event-rate baseline. Read-only input
-- for a journaled novelty score; never an input to any live decision.
CREATE TABLE IF NOT EXISTS guard_hourly_baseline (
    camera_id    INTEGER NOT NULL,
    dow_hour     INTEGER NOT NULL CHECK (dow_hour >= 0 AND dow_hour < 168),
    events_ema   REAL    NOT NULL DEFAULT 0,
    updated_at   INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    PRIMARY KEY (camera_id, dow_hour)
);

-- Appearance-signature visit clusters for unrecognized repeat visitors.
-- Identity-matched persons keep their existing repeat counting; this table
-- only accumulates the unknowns the old path ignored.
CREATE TABLE IF NOT EXISTS guard_signature_visit (
    signature  TEXT    NOT NULL PRIMARY KEY,
    visits     INTEGER NOT NULL DEFAULT 0,
    first_seen INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    last_seen  INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);
-- recording the legacy rule verdict next to the belief verdict. The journal
-- never fails the saga; writes are idempotent by event_id.
CREATE TABLE IF NOT EXISTS guard_decision_journal (
    event_id            TEXT    NOT NULL PRIMARY KEY,
    encounter_id        INTEGER NOT NULL DEFAULT 0,
    incident_id         INTEGER NOT NULL DEFAULT 0,
    camera_id           INTEGER NOT NULL DEFAULT 0,
    observation_id      TEXT    NOT NULL DEFAULT '',
    severity            TEXT    NOT NULL DEFAULT 'none'
                          CHECK (severity IN ('none', 'low', 'medium', 'high',
                                 'critical')),
    severity_rank       INTEGER NOT NULL DEFAULT 0,
    hard_floor          INTEGER NOT NULL DEFAULT 0,
    belief_score        INTEGER NOT NULL DEFAULT 0,
    belief_signals      TEXT    NOT NULL DEFAULT '[]',
    belief_threshold    INTEGER NOT NULL DEFAULT 0,
    legacy_would_notify INTEGER NOT NULL DEFAULT 0,
    belief_would_notify INTEGER NOT NULL DEFAULT 0,
    did_notify          INTEGER NOT NULL DEFAULT 0,
    decision_mode       TEXT    NOT NULL DEFAULT 'shadow'
                          CHECK (decision_mode IN ('shadow', 'enforce')),
    suppression_reason  TEXT    NOT NULL DEFAULT 'none'
                          CHECK (suppression_reason IN ('none', 'belief_gate',
                                 'budget', 'legacy_silent',
                                 'thread_suppressed', 'staging')),
    suppressed_kinds    TEXT    NOT NULL DEFAULT '[]',
    dispatch_attempts   INTEGER NOT NULL DEFAULT 0,
    novelty_score       REAL    NOT NULL DEFAULT 0,
    repeat_visits       INTEGER NOT NULL DEFAULT 0,
    quiet_hold          INTEGER NOT NULL DEFAULT 0,
    budget_hold         INTEGER NOT NULL DEFAULT 0,
    assess_ms           INTEGER NOT NULL DEFAULT 0,
    feedback_label      TEXT    NOT NULL DEFAULT ''
                          CHECK (feedback_label IN ('', 'useful', 'false_alarm',
                                 'not_now')),
    feedback_at         INTEGER NOT NULL DEFAULT 0,
    created_at          INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_guard_decision_journal_encounter
    ON guard_decision_journal (encounter_id, event_id);

-- Cursor pagination and time-range scans (decisions pages, summary bounds).
CREATE INDEX IF NOT EXISTS idx_guard_decision_journal_cursor
    ON guard_decision_journal (created_at DESC, event_id DESC);

-- Per-camera time aggregates (byCameraDay, byCameraHour).
CREATE INDEX IF NOT EXISTS idx_guard_decision_journal_camera_time
    ON guard_decision_journal (camera_id, created_at DESC);
