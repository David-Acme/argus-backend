#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;

#ifndef ARGUS_CAMERA_SCHEMA_PATH
#define ARGUS_CAMERA_SCHEMA_PATH "services/camera/database/schema.sql"
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

CameraResult applyCameraSchema(const CameraSchemaInput& input);

CameraMigrationReport verifyCameraTables(const CameraVerificationInput& input);

CameraMigrationReport migrateCamera(const CameraMigrationOptions& options);
