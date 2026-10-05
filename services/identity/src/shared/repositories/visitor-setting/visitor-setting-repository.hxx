#pragma once

#include "visitor-setting-query.hxx"

#include <drogon/utils/coroutine.h>

class VisitorSettingRepository
{
public:
  [[nodiscard]] drogon::Task<VisitorSetting> find() const;
  drogon::Task<VisitorSetting> update(const VisitorSettingUpdateInput& input) const;
};
