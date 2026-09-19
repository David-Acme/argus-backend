#pragma once

#include <string>

struct CameraDbConfig
{
  std::string dbPath;
  std::string schemaPath;
};

class CameraConfig
{
public:
  // Resolves the [camera] db and schema with the phase-2 defaults.
  static CameraDbConfig resolveDb();
};
