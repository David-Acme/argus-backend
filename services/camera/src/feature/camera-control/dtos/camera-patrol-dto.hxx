#pragma once

#include <feature/camera-control/services/patrol-plan.hxx>

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <string>
#include <validation/validation_dsl.hxx>
#include <vector>

struct CameraPatrolDto
{
  std::optional<std::vector<std::string>> stops;
  std::optional<int64_t> dwellSeconds;
  std::optional<bool> enabled;
  bool windowGiven{false};
  std::optional<PatrolWindow> window;
  bool enabledMalformed{false};

  static CameraPatrolDto fromJson(const Json::Value& json);
};
