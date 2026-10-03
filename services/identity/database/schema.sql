PRAGMA journal_mode       = WAL;
PRAGMA synchronous        = NORMAL;
PRAGMA busy_timeout       = 5000;
PRAGMA cache_size         = -64000;
PRAGMA temp_store         = MEMORY;
PRAGMA mmap_size          = 268435456;
PRAGMA foreign_keys       = ON;
PRAGMA journal_size_limit = 67108864;

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

CREATE TABLE IF NOT EXISTS person (
    id             INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id        INTEGER           REFERENCES user(id) ON DELETE SET NULL,
    name           TEXT    NOT NULL  DEFAULT '',
    alias          TEXT    NOT NULL  DEFAULT '',
    observation    TEXT    NOT NULL  DEFAULT '',
    status         TEXT    NOT NULL  DEFAULT 'known'
                           CHECK (status IN ('candidate', 'known')),
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

CREATE TABLE IF NOT EXISTS voiceprint (
    id              INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    user_id         INTEGER NOT NULL UNIQUE REFERENCES user(id) ON DELETE CASCADE,
    model           TEXT    NOT NULL,
    embedding       BLOB    NOT NULL,
    sample_count    INTEGER NOT NULL CHECK (sample_count BETWEEN 1 AND 10),
    speech_seconds  REAL    NOT NULL CHECK (speech_seconds > 0),
    method          TEXT    NOT NULL CHECK (method IN ('self', 'owner_face')),
    consent_version TEXT    NOT NULL,
    enrolled_by     INTEGER REFERENCES user(id) ON DELETE SET NULL,
    created_at      INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS voiceprint_challenge (
    id              INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    token_hash      TEXT    NOT NULL UNIQUE,
    user_id         INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,
    requester_id    INTEGER NOT NULL REFERENCES user(id) ON DELETE CASCADE,
    device_hash     TEXT    NOT NULL,
    lang            TEXT    NOT NULL CHECK (lang IN ('es', 'en')),
    phrases         TEXT    NOT NULL,
    expires_at      INTEGER NOT NULL,
    consumed_at     INTEGER,
    created_at      INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS voiceprint_challenge_sample (
    challenge_id    INTEGER NOT NULL REFERENCES voiceprint_challenge(id) ON DELETE CASCADE,
    position        INTEGER NOT NULL CHECK (position BETWEEN 0 AND 9),
    embedding       BLOB    NOT NULL,
    speech_seconds  REAL    NOT NULL CHECK (speech_seconds > 0),
    created_at      INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    PRIMARY KEY (challenge_id, position)
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

CREATE INDEX IF NOT EXISTS idx_voiceprint_model
    ON voiceprint (model);

CREATE INDEX IF NOT EXISTS idx_voiceprint_challenge_expiry
    ON voiceprint_challenge (expires_at);

CREATE INDEX IF NOT EXISTS idx_change_outbox_status
    ON change_outbox (status, id);
