#pragma once

#include <json/value.h>
#include <optional>
#include <string>

struct UpdateGuardSiteDto
{
  std::optional<std::string> profile;
  std::optional<bool> scheduleEnabled;
  std::optional<std::string> asleep;
  std::optional<std::string> open;
  std::optional<std::string> staffed;
  std::optional<std::string> closedMode;
  std::optional<int> digestHour;
  std::string invalidTypes;

  static UpdateGuardSiteDto fromJson(const Json::Value& json);
};
