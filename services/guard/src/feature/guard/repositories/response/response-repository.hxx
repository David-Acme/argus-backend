#pragma once

#include "response-query.hxx"

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <vector>

class ResponseRepository
{
public:
  [[nodiscard]] drogon::Task<ResponseConfigRows>
  forEnvironment(int64_t environmentId) const;

  [[nodiscard]] drogon::Task<void>
  replace(const ResponseReplaceInput& input) const;

  [[nodiscard]] drogon::Task<void>
  setDuty(const ResponseDutyInput& input) const;

  [[nodiscard]] drogon::Task<std::vector<int64_t>>
  dutyEnvironments(int64_t userId) const;

private:
  [[nodiscard]] drogon::Task<void>
  writeReplace(const ResponseReplaceInput& input,
               drogon::orm::DbClient* client) const;
};
