#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <shared/validation/validation_dsl.hxx>

struct CameraPtzDto
{
  /** Absolute motor target; ignored when `angle` is present. */
  std::optional<int64_t> x;
  std::optional<int64_t> y;
  /** Tapo protocol direction in degrees (0..359). */
  std::optional<int64_t> angle;

  static CameraPtzDto fromJson(const Json::Value& json);
};
