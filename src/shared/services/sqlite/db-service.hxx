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

  static bool runScriptFile(const std::string& path);
  static bool migrate(int64_t targetVersion);
  static void applyPragmas();
  static void installExtensions();
};
