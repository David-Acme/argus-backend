#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

#ifndef ARGUS_IDENTITY_SCHEMA_PATH
#define ARGUS_IDENTITY_SCHEMA_PATH "argus-identity/database/schema.sql"
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

// Creates the identity tables on db from the schema file, failing on the first statement error.
IdentityResult applyIdentitySchema(const IdentitySchemaInput& input);

// Verifies the attached read-only source ("src") against the target per identity table.
IdentityMigrationReport verifyIdentityTables(const IdentityVerificationInput& input);

// Full migration: schema, transactional copy, verification; the target file is recreated.
IdentityMigrationReport migrateIdentity(const IdentityMigrationOptions& options);
