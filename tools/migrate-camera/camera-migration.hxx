#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

#ifndef ARGUS_CAMERA_SCHEMA_PATH
#define ARGUS_CAMERA_SCHEMA_PATH "database/camera-schema.sql"
#endif

struct CameraMigrationOptions
{
  std::string sourcePath;
  std::string targetPath;
  std::string schemaPath = ARGUS_CAMERA_SCHEMA_PATH;
};

struct CameraTableReport
{
  std::string table;
  int64_t sourceRows = 0;
  int64_t targetRows = 0;
  std::string sourceChecksum;
  std::string targetChecksum;
};

struct CameraMigrationReport
{
  bool ok = false;
  // True when the target already existed schema-current and nothing was
  // copied (the F2-1 Ruling V no-op: after the cutover camera.db is live
  // data and a rerun must never wipe it).
  bool noop = false;
  std::string error;
  std::vector<CameraTableReport> tables;
};

struct CameraSchemaInput
{
  sqlite3* db = nullptr;
  std::string schemaPath;
};

struct CameraResult
{
  bool ok = false;
  std::string error;
};

struct CameraVerificationInput
{
  sqlite3* target = nullptr;
};

// Creates the camera tables on db from the schema file, failing on the first
// statement error.
CameraResult applyCameraSchema(const CameraSchemaInput& input);

// Compares the attached read-only source ("src") against the target main
// database per camera table: column shape, row count and checksum. Requires
// that a source database is already attached as "src".
CameraMigrationReport verifyCameraTables(const CameraVerificationInput& input);

// Full migration: schema creation, transactional copy of the camera tables
// (camera -> camera_stream -> zone, keeping camera_id FK integrity) and
// verification. The source database is attached read-only; the target file is
// recreated. A schema-current target short-circuits to a verified no-op.
CameraMigrationReport migrateCamera(const CameraMigrationOptions& options);
