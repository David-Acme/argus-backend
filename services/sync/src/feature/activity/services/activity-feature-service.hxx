#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/activity/dtos/list-activity-dto.hxx>
#include <optional>
#include <shared/repositories/user-action-log/user-action-log-repository.hxx>
#include <string>
#include <vector>

struct ActivityPage
{
  std::vector<UserActionLogSchema> items;
  std::optional<std::string> nextCursor;
};

namespace activity_cursor
{
[[nodiscard]] std::string encode(const ActivityCursor& cursor);
[[nodiscard]] std::optional<ActivityCursor> decode(const std::string& text);
}

class ActivityFeatureService
{
public:
  drogon::Task<ActivityPage> list(const ListActivityDto& query) const;

private:
  UserActionLogRepository repository_;
};
