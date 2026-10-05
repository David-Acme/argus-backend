#pragma once

#include "response-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>

class ResponseRepository
{
public:
  [[nodiscard]] drogon::Task<ResponseConfigRows>
  forEnvironment(int64_t environmentId) const;

  [[nodiscard]] drogon::Task<void>
  replace(const ResponseReplaceInput& input) const;

  [[nodiscard]] drogon::Task<void>
  setDuty(const ResponseDutyInput& input) const;
};
