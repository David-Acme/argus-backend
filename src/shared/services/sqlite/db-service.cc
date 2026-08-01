#include "db-service.hxx"

#include <fstream>
#include <sstream>
#include <vector>

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

const std::vector<std::string>* migrationStatements(int64_t version)
{
  switch (version) {
    case 1:
      return &kMigrationStatementsV1;
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
