#pragma once

#include "scheduled-call-query.hxx"

#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <drogon/utils/coroutine.h>

#include <vector>

struct ScheduledCallSchema
{
  int64_t id{0};
  int64_t userId{0};
  std::string commandId;
  int64_t fireAt{0};
  std::string topic;
  std::string lang;

  static ScheduledCallSchema fromRow(const drogon::orm::Row& row);
};

class ScheduledCallRepository
{
public:
  ScheduledCallRepository() = default;

  drogon::Task<ScheduledCallCreateResult>
  create(const ScheduledCallCreateInput& input) const;

  drogon::Task<int64_t> pendingCount(int64_t userId) const;

  drogon::Task<std::vector<ScheduledCallSchema>>
  due(const ScheduledCallDueInput& input) const;

  drogon::Task<bool> markFired(int64_t id, int64_t now) const;
};
