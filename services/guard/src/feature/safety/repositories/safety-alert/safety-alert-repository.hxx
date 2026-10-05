#pragma once

#include "safety-alert-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <vector>

class SafetyAlertRepository
{
public:
  [[nodiscard]] drogon::Task<int64_t> insert(const SafetyAlertInsertInput& input) const;

  [[nodiscard]] drogon::Task<std::optional<int64_t>>
  recent(const SafetyAlertRecentInput& input) const;

  [[nodiscard]] drogon::Task<std::vector<SafetyAlertRow>> pending(int64_t since) const;

  drogon::Task<void> markNotified(int64_t id, int64_t now) const;
};
