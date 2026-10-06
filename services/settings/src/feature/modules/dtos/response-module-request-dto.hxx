#pragma once

#include <feature/modules/services/module-engine.hxx>
#include <json/value.h>

struct ResponseModuleRequestDto
{
  ModuleRequestView request;

  [[nodiscard]] Json::Value toJson() const;
};
