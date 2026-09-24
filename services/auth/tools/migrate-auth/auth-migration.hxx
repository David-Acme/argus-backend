#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

#ifndef ARGUS_AUTH_SCHEMA_PATH
#define ARGUS_AUTH_SCHEMA_PATH "services/auth/database/schema.sql"
#endif

struct AuthMigrationOptions
{
  std::string sourcePath;
  std::string targetPath;
  std::string schemaPath = ARGUS_AUTH_SCHEMA_PATH;
};

struct AuthTableReport
{
  std::string table;
  int64_t sourceRows = 0;
  int64_t targetRows = 0;
  std::string sourceChecksum;
  std::string targetChecksum;
};

struct AuthMigrationReport
{
  bool ok = false;
  std::string error;
  std::vector<AuthTableReport> tables;
};

struct AuthSchemaInput
{
  sqlite3* db = nullptr;
  std::string schemaPath;
};

struct AuthResult
{
  bool ok = false;
  std::string error;
};

struct AuthVerificationInput
{
  sqlite3* target = nullptr;
};

AuthResult applyAuthSchema(const AuthSchemaInput& input);

AuthMigrationReport verifyAuthTables(const AuthVerificationInput& input);

AuthMigrationReport migrateAuth(const AuthMigrationOptions& options);
