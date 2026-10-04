#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <validation/validation_dsl.hxx>

struct CameraPtzDto
{
  static constexpr int64_t kMaxStepDegrees = 180;

  std::optional<int64_t> x;
  std::optional<int64_t> y;
  std::optional<int64_t> angle;
  bool stop{false};

  static CameraPtzDto fromJson(const Json::Value& json);
};
