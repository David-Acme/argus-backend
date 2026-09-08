#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

#ifndef ARGUS_NOTIFICATION_SCHEMA_PATH
#define ARGUS_NOTIFICATION_SCHEMA_PATH "database/notification-schema.sql"
#endif

struct NotificationMigrationOptions
{
  std::string sourcePath;
  std::string targetPath;
  std::string schemaPath = ARGUS_NOTIFICATION_SCHEMA_PATH;
};

struct NotificationTableReport
{
  std::string table;
  int64_t sourceRows = 0;
  int64_t targetRows = 0;
  std::string sourceChecksum;
  std::string targetChecksum;
};

struct NotificationMigrationReport
{
  bool ok = false;
  bool noop = false;
  std::string error;
  std::vector<NotificationTableReport> tables;
};

struct NotificationSchemaInput
{
  sqlite3* db = nullptr;
  std::string schemaPath;
};

struct NotificationResult
{
  bool ok = false;
  std::string error;
};

struct NotificationVerificationInput
{
  sqlite3* target = nullptr;
};

// Creates the notification tables on db from the schema file, failing on the first statement error.
NotificationResult applyNotificationSchema(const NotificationSchemaInput& input);

// Verifies the attached read-only source ("src") against the target per notification table.
NotificationMigrationReport verifyNotificationTables(
    const NotificationVerificationInput& input);

// Full migration: schema, transactional copy, verification; schema-current targets no-op.
NotificationMigrationReport migrateNotification(
    const NotificationMigrationOptions& options);
