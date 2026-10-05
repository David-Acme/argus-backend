#pragma once

#include <json/value.h>
#include <shared/repositories/visitor-setting/visitor-setting-query.hxx>

struct ResponseVisitorSettingsDto
{
  VisitorSetting setting;
  bool recognitionEnabled{false};

  [[nodiscard]] Json::Value toJson() const;
};
