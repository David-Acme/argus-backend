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
  // Resolves [notifications] db / [notifications] schema with the Fase-3
  // defaults.
  static NotificationDbConfig resolveDb();
};