#pragma once

#include <config/settings-registry.hxx>
#include <json/value.h>

#include <vector>

struct UpdateSettingsDto
{
  std::vector<SettingChange> changes;
  bool wellFormed{true};

  static UpdateSettingsDto fromJson(const Json::Value& json);
};
