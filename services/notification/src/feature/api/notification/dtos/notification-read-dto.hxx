#pragma once

#include <json/value.h>
#include <validation/validation_dsl.hxx>
#include <vector>

struct NotificationReadDto
{
  std::vector<int64_t> ids;

  static NotificationReadDto fromJson(const Json::Value& json);
};
