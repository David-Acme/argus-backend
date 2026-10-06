#pragma once

#include <feature/modules/schemas/module-view.hxx>
#include <json/value.h>

#include <string>
#include <vector>

struct ResponseListModulesDto
{
  std::vector<ModuleView> modules;
  bool owner{false};
  std::string lang;

  [[nodiscard]] Json::Value toJson() const;
};
