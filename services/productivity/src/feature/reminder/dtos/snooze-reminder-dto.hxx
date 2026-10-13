#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <validation/validation_dsl.hxx>

struct SnoozeReminderDto
{
  std::optional<int64_t> minutes;
  bool tomorrow{false};

  static SnoozeReminderDto fromJson(const Json::Value& json);
  static SnoozeReminderDto validated(SnoozeReminderDto dto);
};
