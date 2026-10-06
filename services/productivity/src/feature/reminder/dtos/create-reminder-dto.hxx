#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <string>
#include <validation/validation_dsl.hxx>

struct CreateReminderDto
{
  std::string title;
  std::string description;
  int64_t scheduledAt{0};
  std::optional<std::string> recurrenceRule;

  static CreateReminderDto fromJson(const Json::Value& json);
  static CreateReminderDto validated(CreateReminderDto dto);
};
