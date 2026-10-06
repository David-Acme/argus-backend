#pragma once

#include <json/value.h>
#include <settings/component-vocabulary.hxx>

#include <optional>
#include <string>
#include <vector>

struct UninstallModuleDto
{
  bool keepData{true};
  std::optional<std::string> pin;
  std::vector<RoleReassignment> reassign;
  bool wellFormed{true};
  bool reassignWellFormed{true};

  static UninstallModuleDto fromJson(const Json::Value& json);
};
