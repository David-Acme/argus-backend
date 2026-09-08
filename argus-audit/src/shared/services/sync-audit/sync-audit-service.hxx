#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <shared/enums.hxx>
#include <vector>

struct SyncAuditModuleInput
{
  int64_t recordId{0};
  TableName tableName{TableName::User};
  Json::Value before;
  Json::Value after;
  std::optional<int64_t> actorId;
};

struct SyncAuditUsersInput
{
  int64_t recordId{0};
  TableName tableName{TableName::User};
  Json::Value before;
  Json::Value after;
  std::vector<int64_t> userIds;
};

/** Publishes a compact, field-level sync change after a domain mutation. */
class SyncAuditService
{
public:
  drogon::Task<void> publishModule(const SyncAuditModuleInput& input) const;
  drogon::Task<void> publishUsers(const SyncAuditUsersInput& input) const;
};
