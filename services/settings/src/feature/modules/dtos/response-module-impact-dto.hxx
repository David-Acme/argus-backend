#pragma once

#include <feature/modules/services/module-engine.hxx>
#include <json/value.h>

struct ResponseModuleImpactDto
{
  ModuleImpactView impact;

  [[nodiscard]] Json::Value toJson() const;
};
