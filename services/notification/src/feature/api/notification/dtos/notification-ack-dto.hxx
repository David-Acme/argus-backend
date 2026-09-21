#pragma once

#include <json/value.h>
#include <validation/validation_dsl.hxx>
#include <vector>

struct NotificationAckDto
{
  std::vector<int64_t> notificationIds;

  static NotificationAckDto fromJson(const Json::Value& json);
};
