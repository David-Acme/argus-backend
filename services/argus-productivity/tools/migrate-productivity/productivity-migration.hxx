#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

#ifndef ARGUS_PRODUCTIVITY_SCHEMA_PATH
#define ARGUS_PRODUCTIVITY_SCHEMA_PATH "services/argus-productivity/database/schema.sql"
#endif

struct ProductivityMigrationOptions
{
  std::string sourcePath;
  std::string targetPath;
  std::string schemaPath = ARGUS_PRODUCTIVITY_SCHEMA_PATH;
};

struct ProductivityTableReport
{
  std::string table;
  int64_t sourceRows = 0;
  int64_t targetRows = 0;
  std::string sourceChecksum;
  std::string targetChecksum;
};

struct ProductivityMigrationReport
{
  bool ok = false;
  bool noop = false;
  std::string error;
  std::vector<ProductivityTableReport> tables;
};

struct ProductivitySchemaInput
{
  sqlite3* db = nullptr;
  std::string schemaPath;
};

struct ProductivityResult
{
  bool ok = false;
  std::string error;
};

struct ProductivityVerificationInput
{
  sqlite3* target = nullptr;
};

// Creates the productivity tables on db from the schema file, failing on the first statement error.
ProductivityResult applyProductivitySchema(const ProductivitySchemaInput& input);

// Verifies the attached read-only source ("src") against the target per productivity table.
ProductivityMigrationReport verifyProductivityTables(
    const ProductivityVerificationInput& input);

// Full migration: schema, transactional copy in FK-safe order, verification; schema-current targets no-op.
ProductivityMigrationReport migrateProductivity(
    const ProductivityMigrationOptions& options);
