#pragma once

#include <cstdint>
#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <string>

class DbService
{
public:
  static drogon::orm::DbClientPtr client()
  {
    // Drogon's fast client mode is PostgreSQL/MySQL-only; SQLite clients
    // always go through the shared client pool. A single connection keeps
    // the SQLite serialization cheap (see number_of_connections in
    // config.toml).
    return drogon::app().getDbClient();
  }

  // Read path of the sync domain. The gateway installs the legacy argus.db
  // opened `file:...?mode=ro` (see setReadOnlyClient); hosts that never
  // install one fall back to the default client, so a single compiled read
  // path serves both.
  static drogon::orm::DbClientPtr readOnlyClient();

  // Installs the named read-only client used by the sync read path. Must be
  // called once at boot, before app().run() creates any IO thread.
  static void setReadOnlyClient(drogon::orm::DbClientPtr client);

  // Enables SQLite URI filenames (`file:...?mode=ro`) process-wide. A no-op
  // once SQLite is initialized; must run before the first sqlite3_open.
  static void enableUriFilenames();

  static bool runScriptFile(const std::string& path);
  static bool migrate(int64_t targetVersion);
  static void applyPragmas();
  static void installExtensions();
};