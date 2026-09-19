#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <shared/validation/validation_dsl.hxx>

// /camera/{id}/ptz body: x/y absolute motor target or the Tapo angle in degrees.
struct CameraPtzDto
{
  std::optional<int64_t> x;
  std::optional<int64_t> y;
  std::optional<int64_t> angle;

  static CameraPtzDto fromJson(const Json::Value& json);
};
