-- ─────────────────────────────────────────────────────────────────────────────
-- Argus gateway  ·  Identity schema (identity.db)
-- The 7 identity tables plus the audit and portrait substrate they write to,
-- all copied verbatim from database/schema.sql (source of truth). Applied by
-- tools/migrate-identity and by the gateway.
-- Structure: pragmas → table creation → indexes (grouped by table).
-- ─────────────────────────────────────────────────────────────────────────────

PRAGMA journal_mode       = WAL;
PRAGMA synchronous        = NORMAL;
PRAGMA busy_timeout       = 5000;
PRAGMA cache_size         = -64000;
PRAGMA temp_store         = MEMORY;
PRAGMA mmap_size          = 268435456;
PRAGMA foreign_keys       = ON;
PRAGMA journal_size_limit = 67108864;

-- ── Tables · Identity ────────────────────────────────────────────────────────

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

-- A pending challenge lets a desktop pair with the mobile owner: the desktop
-- creates it, the mobile approves it, the desktop polls it for tokens. Tokens
-- are bound to the desktop's device_hash so its DeviceFilter matches.
CREATE TABLE IF NOT EXISTS device_login_challenge (
    id            INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    challenge_id  TEXT    NOT NULL  UNIQUE,
    device_hash   TEXT    NOT NULL,
    user_agent    TEXT    NOT NULL  DEFAULT '',
    status        TEXT    NOT NULL  DEFAULT 'pending'
                            CHECK (status IN ('pending', 'approved', 'expired')),
    user_id       INTEGER,
    access_token  TEXT,
    refresh_token TEXT,
    expires_at    INTEGER NOT NULL,
    created_at    INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

-- A per-device secret presented via the X-Argus-Device-Credential header
-- (credential identity mode) replaces the source IP in the device hash. Only
-- its SHA-256 is stored; the plaintext is returned once at issuance.
CREATE TABLE IF NOT EXISTS device_credential (
    id            INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id       INTEGER NOT NULL  REFERENCES user(id) ON DELETE CASCADE,
    device_hash   TEXT    NOT NULL,
    secret_hash   TEXT    NOT NULL  UNIQUE,
    is_active     INTEGER NOT NULL  DEFAULT 1  CHECK (is_active IN (0, 1)),
    created_at    INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

-- Invitation tokens are stored only as hashes. A QR code contains the opaque
-- token, while this database can safely retain the invitation audit trail.
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

-- The unique user key makes an accepted enrollment traceable to exactly one
-- invitation. The service records this in the same transaction as enrollment.
CREATE TABLE IF NOT EXISTS invitation_redemption (
    id              INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
    invitation_id   INTEGER NOT NULL REFERENCES user_invitation(id) ON DELETE CASCADE,
    user_id         INTEGER NOT NULL UNIQUE REFERENCES user(id) ON DELETE RESTRICT,
    redeemed_at     INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);

-- ── Tables · Audit and portrait substrate ────────────────────────────────────

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

CREATE TABLE IF NOT EXISTS user_action_log (
    id         INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id    INTEGER NOT NULL  REFERENCES user(id) ON DELETE CASCADE,
    record_id  INTEGER NOT NULL,
    table_name TEXT    NOT NULL,
    action     TEXT    NOT NULL  CHECK (action IN ('create', 'read', 'update', 'delete')),
    old_data   TEXT    NOT NULL  DEFAULT '{}',
    new_data   TEXT    NOT NULL  DEFAULT '{}',
    ip_address TEXT    NOT NULL  DEFAULT '',
    created_at INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

-- ── Indexes ──────────────────────────────────────────────────────────────────

-- face_embedding
CREATE INDEX IF NOT EXISTS idx_face_embedding_person ON face_embedding (person_id);

-- user
CREATE INDEX IF NOT EXISTS idx_user_created_at  ON user (created_at);
CREATE INDEX IF NOT EXISTS idx_user_deleted_at  ON user (deleted_at);

-- refresh_token
CREATE INDEX IF NOT EXISTS idx_refresh_token_user_id   ON refresh_token (user_id);
CREATE INDEX IF NOT EXISTS idx_refresh_token_access    ON refresh_token (access_token);
CREATE INDEX IF NOT EXISTS idx_refresh_token_refresh   ON refresh_token (refresh_token);

-- user_invitation
CREATE UNIQUE INDEX IF NOT EXISTS idx_user_invitation_token_hash
    ON user_invitation (token_hash);
CREATE INDEX IF NOT EXISTS idx_user_invitation_active
    ON user_invitation (expires_at, revoked_at);
CREATE INDEX IF NOT EXISTS idx_user_invitation_creator
    ON user_invitation (created_by, created_at DESC);

-- invitation_redemption
CREATE INDEX IF NOT EXISTS idx_invitation_redemption_invitation
    ON invitation_redemption (invitation_id, redeemed_at DESC);
CREATE UNIQUE INDEX IF NOT EXISTS idx_invitation_redemption_user
    ON invitation_redemption (user_id);

-- stored_file
CREATE UNIQUE INDEX IF NOT EXISTS idx_stored_file_object_key
    ON stored_file (object_key);
CREATE INDEX IF NOT EXISTS idx_stored_file_category_created
    ON stored_file (category, created_at DESC) WHERE deleted_at IS NULL;

-- user_portrait
CREATE UNIQUE INDEX IF NOT EXISTS idx_user_portrait_user_current
    ON user_portrait (user_id);

-- portrait_preview_capability
CREATE INDEX IF NOT EXISTS idx_portrait_preview_capability_lookup
    ON portrait_preview_capability (token_hash, expires_at, consumed_at);

-- audit_log
CREATE INDEX IF NOT EXISTS idx_audit_log_record   ON audit_log (record_id, table_name);
CREATE INDEX IF NOT EXISTS idx_audit_log_table_ts ON audit_log (table_name, event_timestamp);

-- user_audit_log
CREATE INDEX IF NOT EXISTS idx_user_audit_log_user_ts ON user_audit_log (user_id, event_timestamp);
CREATE INDEX IF NOT EXISTS idx_user_audit_log_record   ON user_audit_log (record_id, table_name);
