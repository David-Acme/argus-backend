#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct CreateProjectTaskDto
{
  int64_t projectId{0};
  std::string title;
  std::string status;
  std::string priority;
  std::optional<int64_t> assigneeId;
  std::optional<int64_t> dueAt;
  double sortOrder{0.0};

  static CreateProjectTaskDto fromJson(const Json::Value& json);
};
