#include "db-service.hxx"

#define SQLITE_CORE
#include "sqlite-vec.h"

#include <fstream>
#include <sstream>
#include <vector>

namespace
{
// Read-only client of the sync domain, installed by the host at boot.
drogon::orm::DbClientPtr& g_readOnlyClient()
{
  static drogon::orm::DbClientPtr client;
  return client;
}

// Identity client of the auth reads, installed by the host at boot.
drogon::orm::DbClientPtr& g_identityClient()
{
  static drogon::orm::DbClientPtr client;
  return client;
}

// Camera-domain client (Rulings X/Z), installed by the host at boot.
drogon::orm::DbClientPtr& g_cameraClient()
{
  static drogon::orm::DbClientPtr client;
  return client;
}
} // namespace

namespace
{

std::vector<std::string> splitStatements(const std::string& script)
{
  std::vector<std::string> statements;
  std::string current;
  std::istringstream ss(script);
  std::string line;
  while (std::getline(ss, line)) {
    auto start = line.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
      continue;
    auto end = line.find_last_not_of(" \t\r\n");
    std::string trimmed = line.substr(start, end - start + 1);
    if (trimmed.rfind("--", 0) == 0)
      continue;
    current += trimmed + "\n";
    if (trimmed.back() == ';') {
      statements.push_back(current);
      current.clear();
    }
  }
  if (!current.empty())
    statements.push_back(current);
  return statements;
}

const std::vector<std::string> kMigrationStatementsV1 = {
    "DROP INDEX IF EXISTS \"idx_user_a  ction_log_created\"",
};

const std::vector<std::string> kMigrationStatementsV2 = {
    "CREATE TABLE IF NOT EXISTS device_login_challenge ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,"
    "challenge_id TEXT NOT NULL UNIQUE,"
    "device_hash TEXT NOT NULL,"
    "user_agent TEXT NOT NULL DEFAULT '',"
    "status TEXT NOT NULL DEFAULT 'pending' CHECK (status IN "
    "('pending', 'approved', 'expired')),"
    "user_id INTEGER,"
    "access_token TEXT,"
    "refresh_token TEXT,"
    "expires_at INTEGER NOT NULL,"
    "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))",
};

struct ColumnPatch
{
  std::string table;
  std::string column;
  std::string definition;
};

/**
 * Columns added after a table shipped. `schema.sql` already carries them for a
 * fresh database, so each one is only applied when introspection says it is
 * missing — an `ALTER` cannot express that.
 */
const std::vector<ColumnPatch> kColumnPatches = {
    {"camera", "cloud_username", "TEXT NOT NULL DEFAULT ''"},
    {"camera", "cloud_password", "TEXT NOT NULL DEFAULT ''"},
    {"camera", "driver", "TEXT NOT NULL DEFAULT 'tapo'"},
    {"camera", "icon", "TEXT NOT NULL DEFAULT 'video'"},
};

const std::vector<std::string>* migrationStatements(int64_t version)
{
  switch (version) {
    case 1:
      return &kMigrationStatementsV1;
    case 2:
      return &kMigrationStatementsV2;
    default:
      return nullptr;
  }
}

const std::vector<std::string> kPerBootPragmas = {
    "PRAGMA journal_mode = WAL",
    "PRAGMA synchronous = NORMAL",
    "PRAGMA busy_timeout = 5000",
    "PRAGMA cache_size = -64000",
    "PRAGMA mmap_size = 268435456",
    "PRAGMA foreign_keys = ON",
    "PRAGMA temp_store = MEMORY",
};

} // namespace

void DbService::setReadOnlyClient(drogon::orm::DbClientPtr client)
{
  g_readOnlyClient() = std::move(client);
}

drogon::orm::DbClientPtr DbService::readOnlyClient()
{
  // Installed at boot, before any IO thread exists: no synchronization.
  if (auto client = g_readOnlyClient())
    return client;
  return client();
}

void DbService::setIdentityClient(drogon::orm::DbClientPtr client)
{
  g_identityClient() = std::move(client);
}

drogon::orm::DbClientPtr DbService::identityClient()
{
  // Installed at boot, before any IO thread exists: no synchronization.
  if (auto client = g_identityClient())
    return client;
  return client();
}

void DbService::setCameraClient(drogon::orm::DbClientPtr client)
{
  g_cameraClient() = std::move(client);
}

drogon::orm::DbClientPtr DbService::cameraClient()
{
  // Installed at boot, before any IO thread exists: no synchronization.
  if (auto client = g_cameraClient())
    return client;
  return client();
}

void DbService::enableUriFilenames()
{
  sqlite3_config(SQLITE_CONFIG_URI, 1);
}

void DbService::installExtensions()
{
  const int rc = sqlite3_auto_extension(
      reinterpret_cast<void (*)(void)>(sqlite3_vec_init));
  if (rc == SQLITE_OK) {
    LOG_INFO << "sqlite-vec: vec0 auto-extension registered ("
             << SQLITE_VEC_VERSION << ")";
  } else {
    LOG_WARN << "sqlite-vec: auto_extension registration failed rc=" << rc;
  }
}

void DbService::applyPragmas()
{
  auto client = DbService::client();
  for (const auto& pragma : kPerBootPragmas) {
    try {
      client->execSqlSync(pragma);
    }
    catch (const std::exception& e) {
      LOG_WARN << "SQLite pragma error: " << pragma << " -> " << e.what();
    }
  }
}

bool DbService::runScriptFile(const std::string& path)
{
  std::ifstream file(path);
  if (!file.is_open()) {
    LOG_WARN << "SQLite script not found: " << path;
    return false;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();

  auto client = DbService::client();
  bool ok = true;
  for (const auto& statement : splitStatements(buffer.str())) {
    try {
      client->execSqlSync(statement);
    }
    catch (const std::exception& e) {
      LOG_WARN << "SQLite statement error (" << path << "): " << e.what()
               << " -> " << statement.substr(0, 80);
      ok = false;
    }
  }

  if (ok)
    LOG_INFO << "Applied SQLite script: " << path;
  return ok;
}

bool DbService::migrate(int64_t targetVersion)
{
  auto client = DbService::client();

  try {
    client->execSqlSync(
        "CREATE TABLE IF NOT EXISTS schema_version ("
        "version INTEGER NOT NULL PRIMARY KEY,"
        "applied_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
  }
  catch (const std::exception& e) {
    LOG_WARN << "schema_version table setup failed: " << e.what();
    return false;
  }

  int64_t current = 0;
  try {
    const auto result = client->execSqlSync(
        "SELECT COALESCE(MAX(version), 0) AS v FROM schema_version");
    if (!result.empty())
      current = result.front()["v"].as<int64_t>();
  }
  catch (const std::exception& e) {
    LOG_WARN << "schema_version read failed: " << e.what();
    return false;
  }

  if (current >= targetVersion) {
    LOG_INFO << "Database schema already at version " << current;
    return true;
  }

  if (!runScriptFile("database/schema.sql"))
    return false;

  for (const auto& patch : kColumnPatches) {
    try {
      const auto columns =
          client->execSqlSync("SELECT name FROM pragma_table_info(?)", patch.table);
      bool present = false;
      for (const auto& row : columns) {
        if (row["name"].as<std::string>() == patch.column) {
          present = true;
          break;
        }
      }
      if (!present)
        client->execSqlSync("ALTER TABLE " + patch.table + " ADD COLUMN " +
                            patch.column + " " + patch.definition);
    }
    catch (const std::exception& e) {
      LOG_FATAL << "Column patch " << patch.table << "." << patch.column
                << " failed: " << e.what();
      return false;
    }
  }

  for (int64_t version = current + 1; version <= targetVersion; ++version) {
    const auto* statements = migrationStatements(version);
    if (!statements)
      continue;
    for (const auto& statement : *statements) {
      try {
        client->execSqlSync(statement);
      }
      catch (const std::exception& e) {
        LOG_FATAL << "Database migration v" << version
                  << " failed: " << e.what();
        return false;
      }
    }
  }

  try {
    client->execSqlSync(
        "INSERT OR REPLACE INTO schema_version (version, applied_at) "
        "VALUES (?, strftime('%s', 'now'))",
        targetVersion);
  }
  catch (const std::exception& e) {
    LOG_WARN << "schema_version record failed: " << e.what();
    return false;
  }

  LOG_INFO << "Database schema migrated to version " << targetVersion;
  return true;
}
