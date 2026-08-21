#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct UpdateProjectTaskDto
{
  std::optional<std::string> title;
  std::optional<std::string> status;
  std::optional<std::string> priority;
  std::optional<int64_t> assigneeId;
  std::optional<int64_t> dueAt;
  std::optional<double> sortOrder;

  static UpdateProjectTaskDto fromJson(const Json::Value& json);
};
