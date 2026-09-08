#pragma once

#include <string>

struct ProductivityDbConfig
{
  std::string dbPath;
  std::string schemaPath;
};

class ProductivityConfig
{
public:
  // Resolves the [productivity] db and schema with the phase-3 defaults.
  static ProductivityDbConfig resolveDb();
};
