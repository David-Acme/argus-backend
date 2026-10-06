#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <string>
#include <validation/validation_dsl.hxx>

struct UpdateReminderDto
{
  std::optional<std::string> title;
  std::optional<std::string> description;
  std::optional<int64_t> scheduledAt;
  std::optional<bool> isCompleted;

  static UpdateReminderDto fromJson(const Json::Value& json);
  static UpdateReminderDto validated(UpdateReminderDto dto);
};
