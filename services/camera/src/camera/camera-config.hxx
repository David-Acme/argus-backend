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
  static CameraDbConfig resolveDb();
};
