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

// Productivity-domain client (Ruling AQ), installed by the host at boot.
drogon::orm::DbClientPtr& g_productivityClient()
{
  static drogon::orm::DbClientPtr client;
  return client;
}

// Notification-domain client (Ruling AR), installed by the host at boot.
drogon::orm::DbClientPtr& g_notificationClient()
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
  // No fallback: an uninstalled read-only client means the tables it serves
  // do not exist on this host, and the sync repositories answer empty.
  return g_readOnlyClient();
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

void DbService::setProductivityClient(drogon::orm::DbClientPtr client)
{
  g_productivityClient() = std::move(client);
}

drogon::orm::DbClientPtr DbService::productivityClient()
{
  // Installed at boot, before any IO thread exists: no synchronization.
  if (auto client = g_productivityClient())
    return client;
  return client();
}

void DbService::setNotificationClient(drogon::orm::DbClientPtr client)
{
  g_notificationClient() = std::move(client);
}

drogon::orm::DbClientPtr DbService::notificationClient()
{
  // Installed at boot, before any IO thread exists: no synchronization.
  if (auto client = g_notificationClient())
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
