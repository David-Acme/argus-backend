#pragma once
#include <json/value.h>
#include <optional>
#include <shared/enums.hxx>
#include <shared/utils/json-diff/json-diff.hxx>
#include <string>
#include <string_view>

namespace user_audit_log_query
{
inline constexpr std::string_view INSERT =
    "INSERT INTO user_audit_log (user_id, record_id, table_name, changes, "
    "priority, event_timestamp) VALUES (?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view FIND_EXIST =
    "SELECT * FROM user_audit_log WHERE user_id = ? AND record_id = ? "
    "AND table_name = ? AND event_timestamp >= ? AND event_timestamp <= ? "
    "LIMIT 1";

inline constexpr std::string_view UPDATE_CHANGES =
    "UPDATE user_audit_log SET changes = ?, event_timestamp = ? WHERE id = ?";

inline constexpr std::string_view REMOVE =
    "DELETE FROM user_audit_log WHERE id = ?";

inline constexpr std::string_view FIND_SYNC =
    "SELECT * FROM user_audit_log WHERE user_id = ? "
    "AND event_timestamp >= ? AND event_timestamp <= ? "
    "ORDER BY event_timestamp ASC LIMIT ";

inline constexpr std::string_view FIND_SYNC_FROM =
    "SELECT * FROM user_audit_log WHERE user_id = ? "
    "AND event_timestamp >= ? ORDER BY event_timestamp ASC LIMIT ";

inline constexpr std::string_view FIND_SYNC_TO =
    "SELECT * FROM user_audit_log WHERE user_id = ? "
    "AND event_timestamp <= ? ORDER BY event_timestamp ASC LIMIT ";

inline constexpr std::string_view FIND_SYNC_ALL =
    "SELECT * FROM user_audit_log WHERE user_id = ? "
    "ORDER BY event_timestamp ASC LIMIT ";

inline constexpr std::string_view FIND_LAST_SYNC =
    "SELECT * FROM user_audit_log WHERE user_id = ? "
    "ORDER BY event_timestamp DESC LIMIT 1";
} // namespace user_audit_log_query

struct UserAuditLogCreateInput
{
  int64_t userId{0};
  int64_t recordId{0};
  TableName tableName{TableName::User};
  Json::Value changes;
  AuditLogPriority priority{AuditLogPriority::Medium};
  int64_t eventTimestamp{0};
};

struct UserAuditLogSyncFilter
{
  int64_t userId{0};
  std::optional<int64_t> startTime;
  std::optional<int64_t> endTime;
};

struct UserAuditLogWriteInput
{
  int64_t userId{0};
  int64_t recordId{0};
  TableName tableName{TableName::User};
  ChangesDiff changes;
  AuditLogPriority priority{AuditLogPriority::Medium};
};

struct UserAuditLogFindExistInput
{
  int64_t userId{0};
  int64_t recordId{0};
  TableName tableName{TableName::User};
  int64_t dayStart{0};
  int64_t dayEnd{0};
};

struct UserAuditLogUpdateInput
{
  int64_t id{0};
  Json::Value changes;
  int64_t eventTimestamp{0};
};
