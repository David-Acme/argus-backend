#pragma once

#include <string>

struct NotificationDbConfig
{
  std::string dbPath;
  std::string schemaPath;
};

class NotificationConfig
{
public:
  static NotificationDbConfig resolveDb();
};
