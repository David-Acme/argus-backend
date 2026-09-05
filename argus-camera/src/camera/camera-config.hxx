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
  // Resolves [camera] db / [camera] schema with the Fase-2 defaults.
  static CameraDbConfig resolveDb();
};
