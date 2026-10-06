#pragma once

#include <http/listener-config.hxx>

#include <cstdint>
#include <string>

struct ProductivityDbConfig
{
  std::string dbPath;
  std::string schemaPath;
};

struct ProductivityNotificationConfig
{
  std::string target;
  std::string credential;
};

struct ProductivityAgendaConfig
{
  bool enabled{true};
  int64_t graceS{120};
};

class ProductivityConfig
{
public:
  static ProductivityDbConfig resolveDb();
  static ListenerConfig resolveListener();
  static ProductivityNotificationConfig resolveNotifications();
  static ProductivityAgendaConfig resolveAgenda();
  static std::string resolveSettingsCredential();
};
