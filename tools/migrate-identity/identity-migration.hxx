#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

#ifndef ARGUS_IDENTITY_SCHEMA_PATH
#define ARGUS_IDENTITY_SCHEMA_PATH "database/identity-schema.sql"
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

// Creates the identity tables on db from the schema file, failing on the first
// statement error.
IdentityResult applyIdentitySchema(const IdentitySchemaInput& input);

// Compares the attached read-only source ("src") against the target main
// database per identity table: column shape, row count and checksum. Requires
// that a source database is already attached as "src".
IdentityMigrationReport verifyIdentityTables(const IdentityVerificationInput& input);

// Full migration: schema creation, transactional copy of the identity tables
// and verification. The source database is attached read-only; the target file
// is recreated.
IdentityMigrationReport migrateIdentity(const IdentityMigrationOptions& options);