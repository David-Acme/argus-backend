#pragma once

#include <feature/settings/services/settings-gateway-service.hxx>
#include <json/value.h>

struct ResponseUpdateSettingsDto
{
  const SettingsUpdateOutcome& outcome;

  [[nodiscard]] Json::Value toJson() const;
};
