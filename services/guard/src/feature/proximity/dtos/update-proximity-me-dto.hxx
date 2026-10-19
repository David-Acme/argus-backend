#pragma once

#include <json/value.h>

struct UpdateProximityMeDto
{
  bool ble{false};

  static UpdateProximityMeDto fromJson(const Json::Value& json);
};
