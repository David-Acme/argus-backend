#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct CreateProjectDto
{
  std::string name;
  std::string description;
  std::string status;
  std::string color;
  std::optional<int64_t> startsAt;
  std::optional<int64_t> targetAt;

  static CreateProjectDto fromJson(const Json::Value& json);
};
