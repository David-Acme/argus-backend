#pragma once

#include <feature/modules/services/module-engine.hxx>
#include <json/value.h>

#include <vector>

struct ResponseModuleDataDto
{
  std::vector<OwnerDataView> owners;

  [[nodiscard]] Json::Value toJson() const;
};
