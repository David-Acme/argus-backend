#pragma once

#include <string>

struct IdentityDbConfig
{
  std::string dbPath;
  std::string schemaPath;
};

class IdentityConfig
{
public:
  // Resolves [identity] db / [identity] schema with the Fase-1 defaults.
  static IdentityDbConfig resolveDb();
};