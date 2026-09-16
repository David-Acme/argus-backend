-- ─────────────────────────────────────────────────────────────────────────────
-- Argus gateway  ·  Gateway schema (gateway.db)
-- Degraded-fallback record only. Identity, notification and sync state live
-- in their owners' databases; this file must never gain their tables.
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

-- Every fallback-path non-delivery while guard is absent: what was dropped
-- and why. Guard-ready handoffs are recorded by guard, never here.
CREATE TABLE IF NOT EXISTS gateway_fallback_event (
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

CREATE INDEX IF NOT EXISTS idx_gateway_fallback_created
    ON gateway_fallback_event (created_at DESC);
