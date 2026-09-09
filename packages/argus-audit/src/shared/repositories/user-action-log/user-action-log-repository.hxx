#pragma once
#include "user-action-log-query.hxx"

#include <drogon/utils/coroutine.h>
#include <shared/schemas/user-action-log/user-action-log-schema.hxx>

class UserActionLogRepository
{
public:
  UserActionLogRepository() = default;

  drogon::Task<UserActionLogSchema>
  create(const UserActionLogCreateInput& input) const;
};
