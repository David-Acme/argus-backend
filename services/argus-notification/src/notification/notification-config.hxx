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
  // Resolves the [notifications] db and schema with the phase-3 defaults.
  static NotificationDbConfig resolveDb();
};
