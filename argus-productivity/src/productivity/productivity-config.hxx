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
  // Resolves [productivity] db / [productivity] schema with the Fase-3
  // defaults.
  static ProductivityDbConfig resolveDb();
};