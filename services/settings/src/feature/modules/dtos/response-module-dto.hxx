#pragma once

#include <feature/modules/schemas/module-view.hxx>
#include <json/value.h>

#include <string>

struct ResponseModuleDto
{
  ModuleView module;
  std::string lang;

  [[nodiscard]] Json::Value toJson() const;
};

struct ResponseModuleJobDto
{
  JobView job;

  [[nodiscard]] Json::Value toJson() const;
};
