#pragma once

#include "wind-down-query.hxx"

#include <drogon/utils/coroutine.h>

class WindDownRepository
{
public:
  [[nodiscard]] drogon::Task<WindDownReport> run(const WindDownInput& input) const;
  [[nodiscard]] drogon::Task<WindDownReport> pending() const;
};
