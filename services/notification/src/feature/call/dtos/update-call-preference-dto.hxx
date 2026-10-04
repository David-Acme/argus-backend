#pragma once

#include <json/value.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct UpdateCallPreferenceDto
{
  std::optional<bool> enabled;
  std::optional<std::string> guardCritical;
  std::optional<std::string> guardIntruder;
  std::optional<std::string> guardEscalation;
  std::optional<std::string> guardArrival;
  std::optional<std::string> agenda;
  std::optional<std::string> assistant;
  std::optional<int> quietStartHour;
  std::optional<int> quietEndHour;
  std::optional<int64_t> dndUntil;
  std::optional<bool> criticalBypass;
  std::optional<std::vector<int64_t>> mutedEnvironmentIds;
  std::optional<int> agendaLeadMinutes;
  std::optional<int> quietDays;
  std::optional<int> ringSeconds;
  std::optional<int> pushDelaySeconds;
  std::optional<bool> liveAnnounce;
  std::optional<std::string> lang;
  std::string invalidTypes;

  static UpdateCallPreferenceDto fromJson(const Json::Value& json);
};
