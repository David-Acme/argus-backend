#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

#ifndef ARGUS_PRODUCTIVITY_SCHEMA_PATH
#define ARGUS_PRODUCTIVITY_SCHEMA_PATH "database/productivity-schema.sql"
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
  // True when the target already existed schema-current and nothing was
  // copied: after the F3-2 cutover productivity.db is live data and a rerun
  // must never wipe it.
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

// Creates the productivity tables on db from the schema file, failing on the
// first statement error.
ProductivityResult applyProductivitySchema(const ProductivitySchemaInput& input);

// Compares the attached read-only source ("src") against the target main
// database per productivity table: column shape, row count and checksum.
// Requires that a source database is already attached as "src".
ProductivityMigrationReport verifyProductivityTables(
    const ProductivityVerificationInput& input);

// Full migration: schema creation, transactional copy of the 7 productivity
// tables (FK-safe order: parents before their project/reminder/calendar_event
// children) and verification. The source database is attached read-only; the
// target file is recreated. A schema-current target short-circuits to a
// verified no-op. The user parent rows live in identity.db, so the copy runs
// with foreign keys off and foreign_key_check ignores user references.
ProductivityMigrationReport migrateProductivity(
    const ProductivityMigrationOptions& options);
