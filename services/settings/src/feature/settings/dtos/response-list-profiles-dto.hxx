#pragma once

#include <feature/settings/services/settings-profile.hxx>
#include <json/value.h>

struct ResponseListProfilesDto
{
  const ProfilesOverview& overview;

  [[nodiscard]] Json::Value toJson() const;
};
