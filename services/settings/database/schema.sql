CREATE TABLE IF NOT EXISTS module_state (
  module_id TEXT PRIMARY KEY NOT NULL,
  lifecycle TEXT NOT NULL DEFAULT 'not_installed'
    CHECK (lifecycle IN ('not_installed', 'active', 'disabled', 'uninstalled_data_kept')),
  data_purged_at INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS module_job (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  module_id TEXT NOT NULL,
  kind TEXT NOT NULL DEFAULT 'install' CHECK (kind IN ('install', 'uninstall', 'purge')),
  owner TEXT NOT NULL DEFAULT '',
  state TEXT NOT NULL CHECK (state IN ('queued', 'checking', 'downloading', 'verifying', 'activating',
                                       'health_check', 'removing', 'purging', 'done', 'paused', 'failed',
                                       'cancelled')),
  reason TEXT NOT NULL DEFAULT '',
  bytes_done INTEGER NOT NULL DEFAULT 0,
  bytes_total INTEGER NOT NULL DEFAULT 0,
  requested_by INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL,
  state_since INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_module_job_module ON module_job (module_id, id);

CREATE INDEX IF NOT EXISTS idx_module_job_open ON module_job (id)
  WHERE state NOT IN ('done', 'failed', 'cancelled');

CREATE TABLE IF NOT EXISTS module_purge (
  module_id TEXT NOT NULL,
  owner TEXT NOT NULL,
  purged_at INTEGER NOT NULL,
  PRIMARY KEY (module_id, owner)
);

CREATE TABLE IF NOT EXISTS module_audit (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  module_id TEXT NOT NULL,
  action TEXT NOT NULL CHECK (action IN ('adopted', 'install_requested', 'enabled', 'disabled', 'rolled_back',
                                         'paused', 'resumed', 'cancelled', 'failed', 'uninstall_requested',
                                         'removed', 'purged')),
  user_id INTEGER NOT NULL DEFAULT 0,
  detail TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_module_audit_module ON module_audit (module_id, id);

CREATE TABLE IF NOT EXISTS module_journal (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  published_through INTEGER NOT NULL DEFAULT 0
);
