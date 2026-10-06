#pragma once

#include <json/value.h>

#include <optional>
#include <string>

struct UninstallModuleDto
{
  bool keepData{true};
  std::optional<std::string> pin;
  bool wellFormed{true};

  static UninstallModuleDto fromJson(const Json::Value& json);
};
