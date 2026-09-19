#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct UpdateProjectDto
{
  std::optional<std::string> name;
  std::optional<std::string> description;
  std::optional<std::string> status;
  std::optional<std::string> color;
  std::optional<int64_t> startsAt;
  std::optional<int64_t> targetAt;

  static UpdateProjectDto fromJson(const Json::Value& json);
};
