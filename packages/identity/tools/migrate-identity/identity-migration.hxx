#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

#ifndef ARGUS_IDENTITY_SCHEMA_PATH
#define ARGUS_IDENTITY_SCHEMA_PATH "packages/identity/database/schema.sql"
#endif

struct IdentityMigrationOptions
{
  std::string sourcePath;
  std::string targetPath;
  std::string schemaPath = ARGUS_IDENTITY_SCHEMA_PATH;
};

struct IdentityTableReport
{
  std::string table;
  int64_t sourceRows = 0;
  int64_t targetRows = 0;
  std::string sourceChecksum;
  std::string targetChecksum;
};

struct IdentityMigrationReport
{
  bool ok = false;
  std::string error;
  std::vector<IdentityTableReport> tables;
};

struct IdentitySchemaInput
{
  sqlite3* db = nullptr;
  std::string schemaPath;
};

struct IdentityResult
{
  bool ok = false;
  std::string error;
};

struct IdentityVerificationInput
{
  sqlite3* target = nullptr;
};

IdentityResult applyIdentitySchema(const IdentitySchemaInput& input);

IdentityMigrationReport verifyIdentityTables(const IdentityVerificationInput& input);

IdentityMigrationReport migrateIdentity(const IdentityMigrationOptions& options);
