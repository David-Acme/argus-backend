#pragma once

#include "safety-setting-query.hxx"

#include <drogon/utils/coroutine.h>

class SafetySettingRepository
{
public:
  [[nodiscard]] drogon::Task<SafetySetting> find() const;

  drogon::Task<void> update(const SafetySettingUpdateInput& input) const;
};
