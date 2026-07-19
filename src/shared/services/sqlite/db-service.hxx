#pragma once

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

  // Run a DDL/SQL file (e.g. database/schema.sql) at startup.
  static drogon::Task<void> runScriptFile(const std::string& path);
};
