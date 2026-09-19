#include "user-action-log-service.hxx"

drogon::Task<void>
UserActionLogService::record(const UserActionLogCreateInput& input) const
{
  co_await repository_.create(input);
  co_return;
}
