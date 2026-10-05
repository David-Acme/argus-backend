PRAGMA journal_mode       = WAL;
PRAGMA synchronous        = NORMAL;
PRAGMA busy_timeout       = 5000;
PRAGMA cache_size         = -64000;
PRAGMA temp_store         = MEMORY;
PRAGMA mmap_size          = 268435456;
PRAGMA foreign_keys       = ON;
PRAGMA journal_size_limit = 67108864;

CREATE TABLE IF NOT EXISTS refresh_token (
    id            INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id       INTEGER NOT NULL,
    access_token  TEXT    NOT NULL,
    refresh_token TEXT    NOT NULL,
    device_hash   TEXT    NOT NULL,
    user_agent    TEXT    NOT NULL  DEFAULT '',
    is_valid      INTEGER NOT NULL  DEFAULT 1  CHECK (is_valid IN (0, 1)),
    is_used       INTEGER NOT NULL  DEFAULT 0  CHECK (is_used  IN (0, 1)),
    expires_at    INTEGER NOT NULL,
    created_at    INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    session_id    TEXT    NOT NULL  DEFAULT '',
    platform      TEXT    NOT NULL  DEFAULT 'unknown'
                            CHECK (platform IN ('unknown', 'android', 'ios',
                                                'desktop', 'web')),
    device_name   TEXT    NOT NULL  DEFAULT '',
    session_created_at     INTEGER NOT NULL  DEFAULT 0,
    last_seen_at           INTEGER NOT NULL  DEFAULT 0,
    previous_refresh_token TEXT    NOT NULL  DEFAULT '',
    network_hash           TEXT    NOT NULL  DEFAULT ''
);

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
    created_at    INTEGER NOT NULL  DEFAULT (strftime('%s', 'now')),
    platform      TEXT    NOT NULL  DEFAULT 'unknown'
                            CHECK (platform IN ('unknown', 'android', 'ios',
                                                'desktop', 'web')),
    device_name   TEXT    NOT NULL  DEFAULT '',
    poll_hash     TEXT    NOT NULL  DEFAULT '',
    origin        TEXT    NOT NULL  DEFAULT 'unknown'
                            CHECK (origin IN ('unknown', 'lan', 'tunnel',
                                              'loopback', 'external')),
    ip_address    TEXT    NOT NULL  DEFAULT ''
);

CREATE TABLE IF NOT EXISTS device_credential (
    id            INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id       INTEGER NOT NULL,
    device_hash   TEXT    NOT NULL,
    secret_hash   TEXT    NOT NULL  UNIQUE,
    is_active     INTEGER NOT NULL  DEFAULT 1  CHECK (is_active IN (0, 1)),
    created_at    INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))
);

CREATE TABLE IF NOT EXISTS change_outbox (
    id          INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    event_id    TEXT    NOT NULL  DEFAULT '',
    subject     TEXT    NOT NULL,
    fingerprint TEXT    NOT NULL  DEFAULT '',
    payload     TEXT    NOT NULL,
    status      TEXT    NOT NULL  DEFAULT 'pending'
                        CHECK (status IN ('pending', 'sent')),
    attempts    INTEGER NOT NULL  DEFAULT 0,
    created_at  INTEGER NOT NULL  DEFAULT 0,
    sent_at     INTEGER NOT NULL  DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_refresh_token_user_id ON refresh_token (user_id);
CREATE INDEX IF NOT EXISTS idx_refresh_token_access  ON refresh_token (access_token);
CREATE INDEX IF NOT EXISTS idx_refresh_token_refresh ON refresh_token (refresh_token);
CREATE INDEX IF NOT EXISTS idx_refresh_token_session ON refresh_token (user_id, session_id);

CREATE INDEX IF NOT EXISTS idx_change_outbox_status
    ON change_outbox (status, id);

CREATE UNIQUE INDEX IF NOT EXISTS idx_change_outbox_event_id
    ON change_outbox (event_id) WHERE event_id <> '';
