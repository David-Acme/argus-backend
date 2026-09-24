#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

#ifndef ARGUS_SYNC_SCHEMA_PATH
#define ARGUS_SYNC_SCHEMA_PATH "services/sync/database/schema.sql"
#endif

struct SyncMigrationOptions
{
  std::string sourcePath;
  std::string targetPath;
  std::string schemaPath = ARGUS_SYNC_SCHEMA_PATH;
};

struct SyncTableReport
{
  std::string table;
  bool present = false;
  int64_t copiedRows = 0;
  int64_t skippedRows = 0;
  std::string sourceChecksum;
  std::string targetChecksum;
};

struct SyncMigrationReport
{
  bool ok = false;
  bool sourceReadWrite = false;
  std::string error;
  std::vector<SyncTableReport> tables;
};

struct SyncSchemaInput
{
  sqlite3* db = nullptr;
  std::string schemaPath;
};

struct SyncResult
{
  bool ok = false;
  std::string error;
};

SyncResult applySyncSchema(const SyncSchemaInput& input);

SyncMigrationReport migrateSync(const SyncMigrationOptions& options);
