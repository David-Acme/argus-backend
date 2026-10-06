#pragma once

#include <feature/activity/services/activity-feature-service.hxx>
#include <json/value.h>

struct ResponseActivityDto
{
  ActivityPage page;

  [[nodiscard]] Json::Value toJson() const;
};
