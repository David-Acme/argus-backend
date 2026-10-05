#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>

struct PanicDto
{
  std::optional<int64_t> environmentId;

  static PanicDto fromJson(const Json::Value& json);
};
