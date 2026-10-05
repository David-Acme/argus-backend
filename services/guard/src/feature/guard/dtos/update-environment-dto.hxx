#pragma once

#include <json/value.h>
#include <optional>
#include <string>

struct UpdateEnvironmentDto
{
  std::optional<std::string> name;
  std::optional<std::string> kind;
  std::optional<bool> scheduleEnabled;
  std::optional<std::string> asleep;
  std::optional<std::string> open;
  std::optional<std::string> staffed;
  std::optional<std::string> closedMode;
  std::optional<int> digestHour;
  std::optional<std::string> quietPolicy;
  std::optional<int> quietStartHour;
  std::optional<int> quietEndHour;
  std::optional<bool> lanPresence;
  std::string invalidTypes;

  static UpdateEnvironmentDto fromJson(const Json::Value& json);
};
