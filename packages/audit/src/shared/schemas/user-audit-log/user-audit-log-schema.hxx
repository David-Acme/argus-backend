#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <shared/contracts/audit-log-priority.hxx>
#include <shared/contracts/table-name.hxx>
#include <string>

struct UserAuditLogSchema
{
  int64_t id{0};
  int64_t userId{0};
  int64_t recordId{0};
  TableName tableName{TableName::User};
  Json::Value changes;
  AuditLogPriority priority{AuditLogPriority::Medium};
  int64_t eventTimestamp{0};
  int64_t createdAt{0};

  UserAuditLogSchema() = default;
  explicit UserAuditLogSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
