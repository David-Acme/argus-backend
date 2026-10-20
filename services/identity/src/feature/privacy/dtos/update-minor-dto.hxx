#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>

struct UpdateMinorDto
{
  int64_t userId{0};
  std::optional<bool> minor;
  std::optional<bool> guardianConsent;

  static UpdateMinorDto fromJson(const Json::Value& json, int64_t userId);
};
