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
  static ProductivityDbConfig resolveDb();
};
