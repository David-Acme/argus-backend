#pragma once

#include <feature/presence/services/presence-service.hxx>

#include <json/value.h>
#include <vector>

struct ResponsePresenceDto
{
  std::vector<PresenceUserView> people;

  [[nodiscard]] Json::Value toJson() const;
};
