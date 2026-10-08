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
CREATE INDEX IF NOT EXISTS idx_guard_incident_tamper
    ON guard_incident (created_at DESC) WHERE rule = 'camera_tamper';

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
    environment_id INTEGER NOT NULL DEFAULT 0,
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
    notify_highest_rank INTEGER NOT NULL DEFAULT 0,
    subject          TEXT    NOT NULL  DEFAULT '',
    people           INTEGER NOT NULL  DEFAULT 0,
    reasons          TEXT    NOT NULL  DEFAULT '[]',
    reasons_rank     INTEGER NOT NULL  DEFAULT 0,
    group_id         INTEGER NOT NULL  DEFAULT 0,
    review_label     TEXT    NOT NULL  DEFAULT ''
                             CHECK (review_label IN ('', 'useful', 'false_alarm',
                                    'not_now')),
    reviewed_at      INTEGER NOT NULL  DEFAULT 0,
    environment_id   INTEGER NOT NULL  DEFAULT 0,
    retain_until     INTEGER NOT NULL  DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_guard_encounter_last_seen
    ON guard_encounter (last_seen DESC);
CREATE INDEX IF NOT EXISTS idx_guard_encounter_environment
    ON guard_encounter (environment_id, last_seen DESC);
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

CREATE TABLE IF NOT EXISTS guard_hourly_baseline (
    camera_id    INTEGER NOT NULL,
    dow_hour     INTEGER NOT NULL CHECK (dow_hour >= 0 AND dow_hour < 168),
    events_ema   REAL    NOT NULL DEFAULT 0,
    updated_at   INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    PRIMARY KEY (camera_id, dow_hour)
);

CREATE TABLE IF NOT EXISTS guard_signature_visit (
    signature  TEXT    NOT NULL PRIMARY KEY,
    visits     INTEGER NOT NULL DEFAULT 0,
    first_seen INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    last_seen  INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);
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
                                 'thread_suppressed', 'staging', 'grouped',
                                 'held')),
    suppressed_kinds    TEXT    NOT NULL DEFAULT '[]',
    reasons             TEXT    NOT NULL DEFAULT '[]',
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
    environment_id      INTEGER NOT NULL DEFAULT 0,
    created_at          INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_guard_decision_journal_encounter
    ON guard_decision_journal (encounter_id, event_id);

CREATE INDEX IF NOT EXISTS idx_guard_decision_journal_cursor
    ON guard_decision_journal (created_at DESC, event_id DESC);

CREATE INDEX IF NOT EXISTS idx_guard_decision_journal_camera_time
    ON guard_decision_journal (camera_id, created_at DESC);

CREATE INDEX IF NOT EXISTS idx_guard_decision_journal_environment_time
    ON guard_decision_journal (environment_id, created_at DESC);

CREATE TABLE IF NOT EXISTS guard_environment (
    id               INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    name             TEXT    NOT NULL CHECK (length(name) BETWEEN 1 AND 60),
    kind             TEXT    NOT NULL DEFAULT 'home'
                       CHECK (kind IN ('home', 'office', 'commercial',
                              'restaurant', 'warehouse', 'outdoor')),
    is_default       INTEGER NOT NULL DEFAULT 0 CHECK (is_default IN (0, 1)),
    mode             TEXT    NOT NULL DEFAULT 'home'
                       CHECK (mode IN ('home', 'away', 'night', 'armed')),
    mode_updated_at  INTEGER NOT NULL DEFAULT 0,
    schedule_enabled INTEGER NOT NULL DEFAULT 0 CHECK (schedule_enabled IN (0, 1)),
    asleep_hours     TEXT    NOT NULL DEFAULT '',
    open_hours       TEXT    NOT NULL DEFAULT '',
    staffed_hours    TEXT    NOT NULL DEFAULT '',
    closed_mode      TEXT    NOT NULL DEFAULT 'away'
                       CHECK (closed_mode IN ('away', 'armed')),
    digest_hour      INTEGER NOT NULL DEFAULT 21
                       CHECK (digest_hour >= -1 AND digest_hour <= 23),
    quiet_policy     TEXT    NOT NULL DEFAULT 'inherit'
                       CHECK (quiet_policy IN ('inherit', 'custom', 'off')),
    quiet_start_hour INTEGER NOT NULL DEFAULT 22
                       CHECK (quiet_start_hour >= 0 AND quiet_start_hour <= 23),
    quiet_end_hour   INTEGER NOT NULL DEFAULT 7
                       CHECK (quiet_end_hour >= 0 AND quiet_end_hour <= 23),
    lan_presence     INTEGER NOT NULL DEFAULT 0 CHECK (lan_presence IN (0, 1)),
    created_at       INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    updated_at       INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_guard_environment_default
    ON guard_environment (is_default) WHERE is_default = 1;

CREATE UNIQUE INDEX IF NOT EXISTS idx_guard_environment_name
    ON guard_environment (name COLLATE NOCASE);

CREATE TABLE IF NOT EXISTS guard_camera_context (
    camera_id    INTEGER NOT NULL PRIMARY KEY,
    role         TEXT    NOT NULL DEFAULT 'other'
                   CHECK (role IN ('other', 'entrance', 'perimeter', 'garage',
                          'living', 'kitchen', 'office', 'register', 'storage',
                          'public_area')),
    outdoor      INTEGER NOT NULL DEFAULT 0 CHECK (outdoor IN (0, 1)),
    public_area  INTEGER NOT NULL DEFAULT 0 CHECK (public_area IN (0, 1)),
    active_hours TEXT    NOT NULL DEFAULT '',
    environment_id INTEGER NOT NULL DEFAULT 0,
    updated_at   INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_guard_camera_context_environment
    ON guard_camera_context (environment_id);

CREATE TABLE IF NOT EXISTS guard_presence (
    user_id        INTEGER NOT NULL CHECK (user_id > 0),
    environment_id INTEGER NOT NULL
                     REFERENCES guard_environment (id) ON DELETE CASCADE,
    state          TEXT    NOT NULL CHECK (state IN ('home', 'away')),
    source         TEXT    NOT NULL
                     CHECK (source IN ('lan_session', 'tunnel_session',
                            'app_activity', 'camera', 'timeout')),
    since          INTEGER NOT NULL,
    last_home_at   INTEGER NOT NULL DEFAULT 0,
    last_signal_at INTEGER NOT NULL,
    PRIMARY KEY (user_id, environment_id)
) WITHOUT ROWID;

CREATE INDEX IF NOT EXISTS idx_guard_presence_environment
    ON guard_presence (environment_id, user_id);

CREATE INDEX IF NOT EXISTS idx_guard_presence_home
    ON guard_presence (last_home_at) WHERE state = 'home';

CREATE TABLE IF NOT EXISTS guard_response_recipient (
    environment_id INTEGER NOT NULL CHECK (environment_id > 0),
    user_id        INTEGER NOT NULL CHECK (user_id > 0),
    mode           TEXT    CHECK (mode IS NULL OR mode IN ('call', 'notify', 'off')),
    step           INTEGER CHECK (step IS NULL OR (step >= 0 AND step <= 32)),
    on_duty        INTEGER NOT NULL DEFAULT 0 CHECK (on_duty IN (0, 1)),
    updated_at     INTEGER NOT NULL,
    PRIMARY KEY (environment_id, user_id)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS guard_response_contact (
    id             INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    environment_id INTEGER NOT NULL CHECK (environment_id > 0),
    position       INTEGER NOT NULL DEFAULT 0,
    name           TEXT    NOT NULL CHECK (length(name) BETWEEN 1 AND 60),
    phone          TEXT    NOT NULL CHECK (length(phone) BETWEEN 3 AND 24),
    note           TEXT    NOT NULL DEFAULT '' CHECK (length(note) <= 60),
    updated_at     INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_guard_response_contact_environment
    ON guard_response_contact (environment_id, position);

CREATE TABLE IF NOT EXISTS guard_response_setting (
    environment_id   INTEGER NOT NULL PRIMARY KEY CHECK (environment_id > 0),
    emergency_number TEXT    NOT NULL DEFAULT '' CHECK (length(emergency_number) <= 16),
    step_seconds     INTEGER NOT NULL DEFAULT 45
                       CHECK (step_seconds >= 15 AND step_seconds <= 300),
    updated_at       INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS guard_user_pin (
    user_id      INTEGER PRIMARY KEY,
    disarm_hash  TEXT    NOT NULL,
    duress_hash  TEXT    NOT NULL,
    created_at   INTEGER NOT NULL,
    updated_at   INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS guard_safety_setting (
    id              INTEGER PRIMARY KEY CHECK (id = 1),
    duress_enabled  INTEGER NOT NULL DEFAULT 0 CHECK (duress_enabled IN (0, 1)),
    updated_at      INTEGER NOT NULL DEFAULT 0,
    updated_by      INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS guard_safety_alert (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    kind            TEXT    NOT NULL CHECK (kind IN ('panic', 'duress')),
    user_id         INTEGER NOT NULL,
    environment_id  INTEGER NOT NULL DEFAULT 0,
    created_at      INTEGER NOT NULL,
    notified_at     INTEGER NOT NULL DEFAULT 0,
    notify_sequence INTEGER NOT NULL DEFAULT 1,
    escalated_at    INTEGER NOT NULL DEFAULT 0,
    closed_at       INTEGER NOT NULL DEFAULT 0,
    actor_name      TEXT    NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_guard_safety_alert_user_kind
    ON guard_safety_alert(user_id, kind, created_at);
