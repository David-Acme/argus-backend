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
    return drogon::app().getDbClient();
  }

  static bool runScriptFile(const std::string& path);
  static bool migrate(int64_t targetVersion);
  static void applyPragmas();
};
