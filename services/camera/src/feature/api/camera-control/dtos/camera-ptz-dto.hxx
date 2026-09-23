#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <validation/validation_dsl.hxx>

struct CameraPtzDto
{
  std::optional<int64_t> x;
  std::optional<int64_t> y;
  std::optional<int64_t> angle;

  static CameraPtzDto fromJson(const Json::Value& json);
};
