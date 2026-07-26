#pragma once
#include "user-action-log-query.hxx"

#include <drogon/utils/coroutine.h>
#include <shared/schemas/user-action-log/user-action-log-schema.hxx>

class UserActionLogRepository
{
public:
  UserActionLogRepository() = default;

  drogon::Task<std::vector<UserActionLogSchema>>
  findByUser(int64_t userId, int32_t limit = 50) const;
  drogon::Task<std::vector<UserActionLogSchema>>
  findByRecord(int64_t recordId, const std::string& tableName,
               int32_t limit = 50) const;
};
