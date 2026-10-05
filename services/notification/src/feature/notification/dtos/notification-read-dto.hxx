#pragma once

#include <json/value.h>
#include <validation/validation_dsl.hxx>
#include <cstddef>
#include <vector>

struct NotificationReadDto
{
  static constexpr std::size_t kMaxIdsPerRequest = 500;

  std::vector<int64_t> ids;

  static NotificationReadDto fromJson(const Json::Value& json);
};
