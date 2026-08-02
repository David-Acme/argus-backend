#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/repositories/user-action-log/user-action-log-query.hxx>
#include <shared/repositories/user-action-log/user-action-log-repository.hxx>

class UserActionLogService
{
public:
  UserActionLogService() = default;

  drogon::Task<void> record(const UserActionLogCreateInput& input) const;

private:
  UserActionLogRepository repository_;
};
