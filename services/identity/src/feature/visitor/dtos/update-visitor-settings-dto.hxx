#pragma once

#include <cstdint>
#include <json/value.h>

struct UpdateVisitorSettingsDto
{
  int64_t unnamedRetentionDays{0};

  static UpdateVisitorSettingsDto fromJson(const Json::Value& json);
};
