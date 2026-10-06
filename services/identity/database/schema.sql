PRAGMA journal_mode       = WAL;
PRAGMA synchronous        = NORMAL;
PRAGMA busy_timeout       = 5000;
PRAGMA cache_size         = -64000;
PRAGMA temp_store         = MEMORY;
PRAGMA mmap_size          = 268435456;
PRAGMA foreign_keys       = ON;
PRAGMA journal_size_limit = 67108864;
PRAGMA secure_delete      = ON;

CREATE TABLE IF NOT EXISTS user (
    id             INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    name           TEXT    NOT NULL,
    last_name      TEXT    NOT NULL,
    role           TEXT    NOT NULL,
    lang           TEXT    NOT NULL  DEFAULT 'es'  CHECK (lang IN ('es', 'en')),
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
    status         TEXT    NOT NULL  DEFAULT 'known'
                           CHECK (status IN ('candidate', 'known')),
    category       TEXT    NOT NULL  DEFAULT ''
                           CHECK (category IN ('', 'neighbor', 'delivery', 'service',
                                               'family', 'acquaintance', 'watchlist')),
    note           TEXT    NOT NULL  DEFAULT '',
    visitor_number INTEGER,
    visit_count    INTEGER NOT NULL  DEFAULT 0,
    first_seen_at  INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    last_seen_at   INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    created_at     INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at     INTEGER,
    deleted_at     INTEGER
);

CREATE TABLE IF NOT EXISTS face_embedding (
    id          INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    person_id   INTEGER NOT NULL  REFERENCES person(id) ON DELETE CASCADE,
    embedding   BLOB    NOT NULL,
    angle_label TEXT    NOT NULL  DEFAULT 'frontal',
    quality     REAL    NOT NULL  DEFAULT 1.0,
    model       TEXT    NOT NULL  DEFAULT 'legacy',
    camera_id   INTEGER,
    crop_key    TEXT    NOT NULL  DEFAULT '',
    created_at  INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS person_tag (
    id         INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    person_id  INTEGER NOT NULL  REFERENCES person(id) ON DELETE CASCADE,
    tag        TEXT    NOT NULL,
    source     TEXT    NOT NULL  DEFAULT 'llm',
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    UNIQUE (person_id, tag)
);

CREATE TABLE IF NOT EXISTS person_snapshot (
    id         INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    person_id  INTEGER NOT NULL  UNIQUE REFERENCES person(id) ON DELETE CASCADE,
    image      BLOB    NOT NULL,
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS user_invitation (
    id                INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    token_hash        TEXT    NOT NULL UNIQUE,
    role              TEXT    NOT NULL,
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

CREATE TABLE IF NOT EXISTS invitation_redemption (
    id              INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    invitation_id   INTEGER NOT NULL REFERENCES user_invitation(id) ON DELETE CASCADE,
    user_id         INTEGER NOT NULL UNIQUE REFERENCES user(id) ON DELETE RESTRICT,
    redeemed_at     INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

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

CREATE TABLE IF NOT EXISTS user_portrait (
    id              INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    user_id         INTEGER NOT NULL UNIQUE REFERENCES user(id) ON DELETE CASCADE,
    file_id         INTEGER NOT NULL UNIQUE REFERENCES stored_file(id) ON DELETE RESTRICT,
    created_at      INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    updated_at      INTEGER
);

CREATE TABLE IF NOT EXISTS portrait_preview_capability (
    id                  INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    token_hash          TEXT    NOT NULL UNIQUE,
    portrait_user_id    INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,
    requester_user_id   INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,
    expires_at          INTEGER NOT NULL,
    consumed_at         INTEGER,
    created_at          INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS voice_profile (
    id              INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    user_id         INTEGER NOT NULL UNIQUE REFERENCES user(id) ON DELETE CASCADE,
    model           TEXT    NOT NULL,
    embedding       BLOB    NOT NULL,
    sample_count    INTEGER NOT NULL CHECK (sample_count >= 1),
    speech_seconds  REAL    NOT NULL CHECK (speech_seconds > 0),
    source          TEXT    NOT NULL CHECK (source IN ('passive', 'enrolled')),
    linked_at       INTEGER NOT NULL,
    refreshed_at    INTEGER NOT NULL,
    created_at      INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS voice_sample (
    id              INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    user_id         INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,
    model           TEXT    NOT NULL,
    device_hash     TEXT    NOT NULL,
    embedding       BLOB    NOT NULL,
    turns           INTEGER NOT NULL CHECK (turns >= 1),
    speech_seconds  REAL    NOT NULL CHECK (speech_seconds > 0),
    state           TEXT    NOT NULL CHECK (state IN ('pending', 'adopted')),
    created_at      INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS voice_device (
    device_hash     TEXT    NOT NULL,
    user_id         INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,
    calls           INTEGER NOT NULL DEFAULT 0,
    matched         INTEGER NOT NULL DEFAULT 0,
    conflicting     INTEGER NOT NULL DEFAULT 0,
    mixed           INTEGER NOT NULL DEFAULT 0,
    last_call_at    INTEGER NOT NULL,
    PRIMARY KEY (device_hash, user_id)
);

CREATE TABLE IF NOT EXISTS user_privacy (
    user_id         INTEGER NOT NULL  PRIMARY KEY REFERENCES user(id) ON DELETE CASCADE,
    notice_version  INTEGER NOT NULL  CHECK (notice_version > 0),
    presence        INTEGER NOT NULL  CHECK (presence IN (0, 1)),
    face_cameras    INTEGER NOT NULL  CHECK (face_cameras IN (0, 1)),
    voice_learning  INTEGER NOT NULL  CHECK (voice_learning IN (0, 1)),
    camera_audio    INTEGER NOT NULL  CHECK (camera_audio IN (0, 1)),
    decided_at      INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    updated_at      INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS household_privacy (
    id              INTEGER NOT NULL  PRIMARY KEY CHECK (id = 1),
    presence        INTEGER NOT NULL  DEFAULT 1  CHECK (presence IN (0, 1)),
    face_cameras    INTEGER NOT NULL  DEFAULT 1  CHECK (face_cameras IN (0, 1)),
    voice_learning  INTEGER NOT NULL  DEFAULT 1  CHECK (voice_learning IN (0, 1)),
    camera_audio    INTEGER NOT NULL  DEFAULT 1  CHECK (camera_audio IN (0, 1)),
    visitor_recognition  INTEGER NOT NULL  DEFAULT 0  CHECK (visitor_recognition IN (0, 1)),
    visitor_ack_version  INTEGER,
    visitor_ack_by       INTEGER,
    visitor_ack_at       INTEGER,
    updated_by      INTEGER,
    updated_at      INTEGER
);

INSERT OR IGNORE INTO household_privacy (id) VALUES (1);

CREATE TABLE IF NOT EXISTS person_visit (
    id             INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    person_id      INTEGER NOT NULL  REFERENCES person(id) ON DELETE CASCADE,
    camera_id      INTEGER NOT NULL,
    started_at     INTEGER NOT NULL,
    last_seen_at   INTEGER NOT NULL,
    sightings      INTEGER NOT NULL  DEFAULT 1  CHECK (sightings >= 1)
);

CREATE TABLE IF NOT EXISTS person_crop_capability (
    id                  INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    token_hash          TEXT    NOT NULL UNIQUE,
    person_id           INTEGER NOT NULL REFERENCES person(id) ON DELETE CASCADE,
    face_embedding_id   INTEGER NOT NULL REFERENCES face_embedding(id) ON DELETE CASCADE,
    requester_user_id   INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,
    expires_at          INTEGER NOT NULL,
    consumed_at         INTEGER,
    created_at          INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS visitor_setting (
    id                    INTEGER NOT NULL  PRIMARY KEY CHECK (id = 1),
    unnamed_retention_days INTEGER NOT NULL DEFAULT 30
                                   CHECK (unnamed_retention_days BETWEEN 1 AND 60),
    updated_by            INTEGER,
    updated_at            INTEGER
);

INSERT OR IGNORE INTO visitor_setting (id) VALUES (1);

CREATE TABLE IF NOT EXISTS visitor_counter (
    id           INTEGER NOT NULL  PRIMARY KEY CHECK (id = 1),
    last_number  INTEGER NOT NULL  DEFAULT 0  CHECK (last_number >= 0)
);

INSERT OR IGNORE INTO visitor_counter (id, last_number) VALUES (1, 0);

CREATE TABLE IF NOT EXISTS pending_object_delete (
    id               INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    object_key       TEXT    NOT NULL  UNIQUE,
    attempts         INTEGER NOT NULL  DEFAULT 0  CHECK (attempts >= 0),
    next_attempt_at  INTEGER NOT NULL  DEFAULT 0,
    created_at       INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS change_outbox (
    id          INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    event_id    TEXT              UNIQUE,
    subject     TEXT    NOT NULL,
    fingerprint TEXT    NOT NULL  DEFAULT '',
    payload     TEXT    NOT NULL,
    status      TEXT    NOT NULL  DEFAULT 'pending'
                        CHECK (status IN ('pending', 'sent')),
    attempts    INTEGER NOT NULL  DEFAULT 0,
    created_at  INTEGER NOT NULL  DEFAULT 0,
    sent_at     INTEGER NOT NULL  DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_face_embedding_person ON face_embedding (person_id);

CREATE INDEX IF NOT EXISTS idx_person_tag_person ON person_tag (person_id);
CREATE INDEX IF NOT EXISTS idx_person_visitor_seen ON person (last_seen_at DESC, id DESC)
    WHERE deleted_at IS NULL AND user_id IS NULL;

CREATE INDEX IF NOT EXISTS idx_user_created_at  ON user (created_at);
CREATE INDEX IF NOT EXISTS idx_user_deleted_at  ON user (deleted_at);

CREATE UNIQUE INDEX IF NOT EXISTS idx_user_invitation_token_hash
    ON user_invitation (token_hash);
CREATE INDEX IF NOT EXISTS idx_user_invitation_active
    ON user_invitation (expires_at, revoked_at);
CREATE INDEX IF NOT EXISTS idx_user_invitation_creator
    ON user_invitation (created_by, created_at DESC);

CREATE INDEX IF NOT EXISTS idx_invitation_redemption_invitation
    ON invitation_redemption (invitation_id, redeemed_at DESC);
CREATE UNIQUE INDEX IF NOT EXISTS idx_invitation_redemption_user
    ON invitation_redemption (user_id);

CREATE UNIQUE INDEX IF NOT EXISTS idx_stored_file_object_key
    ON stored_file (object_key);
CREATE INDEX IF NOT EXISTS idx_stored_file_category_created
    ON stored_file (category, created_at DESC) WHERE deleted_at IS NULL;

CREATE UNIQUE INDEX IF NOT EXISTS idx_user_portrait_user_current
    ON user_portrait (user_id);

CREATE INDEX IF NOT EXISTS idx_portrait_preview_capability_lookup
    ON portrait_preview_capability (token_hash, expires_at, consumed_at);

CREATE INDEX IF NOT EXISTS idx_voice_profile_model
    ON voice_profile (model);

CREATE INDEX IF NOT EXISTS idx_voice_sample_user
    ON voice_sample (user_id, model, created_at);

CREATE INDEX IF NOT EXISTS idx_voice_sample_created
    ON voice_sample (state, created_at);

CREATE INDEX IF NOT EXISTS idx_person_visit_person
    ON person_visit (person_id, started_at DESC);
CREATE INDEX IF NOT EXISTS idx_person_visit_open
    ON person_visit (person_id, camera_id, last_seen_at);

CREATE INDEX IF NOT EXISTS idx_person_crop_capability_lookup
    ON person_crop_capability (token_hash, expires_at, consumed_at);

CREATE INDEX IF NOT EXISTS idx_pending_object_delete_due
    ON pending_object_delete (next_attempt_at, id);

CREATE INDEX IF NOT EXISTS idx_change_outbox_status
    ON change_outbox (status, id);
