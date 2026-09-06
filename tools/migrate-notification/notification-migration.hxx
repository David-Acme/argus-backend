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
  // True when the target already existed schema-current and nothing was
  // copied: after the F3-2 cutover notification.db is live data and a rerun
  // must never wipe it.
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

// Creates the notification tables on db from the schema file, failing on the
// first statement error.
NotificationResult applyNotificationSchema(const NotificationSchemaInput& input);

// Compares the attached read-only source ("src") against the target main
// database per notification table: column shape, row count and checksum.
// Requires that a source database is already attached as "src".
NotificationMigrationReport verifyNotificationTables(
    const NotificationVerificationInput& input);

// Full migration: schema creation, transactional copy of notification and
// notification_token and verification. The source database is attached
// read-only; the target file is recreated. A schema-current target
// short-circuits to a verified no-op. The user parent rows live in
// identity.db, so the copy runs with foreign keys off and foreign_key_check
// ignores user references.
NotificationMigrationReport migrateNotification(
    const NotificationMigrationOptions& options);
