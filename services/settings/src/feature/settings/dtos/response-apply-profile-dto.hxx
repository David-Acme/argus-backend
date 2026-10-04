#pragma once

#include <feature/settings/services/settings-profile.hxx>
#include <json/value.h>

struct ResponseApplyProfileDto
{
  const ProfileApplyOutcome& outcome;

  [[nodiscard]] Json::Value toJson() const;
};
