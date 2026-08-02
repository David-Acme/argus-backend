#pragma once
#include <json/value.h>
#include <optional>
#include <shared/enums.hxx>
#include <shared/utils/json-diff/json-diff.hxx>
#include <string>
#include <string_view>
#include <vector>

namespace audit_log_query
{
inline constexpr std::string_view INSERT =
    "INSERT INTO audit_log (create_user_id, record_id, table_name, changes, "
    "priority, event_timestamp) VALUES (?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view FIND_EXIST =
    "SELECT * FROM audit_log WHERE record_id = ? AND table_name = ? "
    "AND event_timestamp >= ? AND event_timestamp <= ? LIMIT 1";

inline constexpr std::string_view UPDATE_CHANGES =
    "UPDATE audit_log SET changes = ?, event_timestamp = ? WHERE id = ?";

inline constexpr std::string_view REMOVE =
    "DELETE FROM audit_log WHERE id = ?";

inline constexpr std::string_view FIND_SYNC =
    "SELECT * FROM audit_log WHERE table_name IN (%1%) "
    "AND event_timestamp >= ? AND event_timestamp <= ? "
    "ORDER BY event_timestamp ASC LIMIT ";

inline constexpr std::string_view FIND_SYNC_FROM =
    "SELECT * FROM audit_log WHERE table_name IN (%1%) "
    "AND event_timestamp >= ? ORDER BY event_timestamp ASC LIMIT ";

inline constexpr std::string_view FIND_SYNC_TO =
    "SELECT * FROM audit_log WHERE table_name IN (%1%) "
    "AND event_timestamp <= ? ORDER BY event_timestamp ASC LIMIT ";

inline constexpr std::string_view FIND_SYNC_ALL =
    "SELECT * FROM audit_log WHERE table_name IN (%1%) "
    "ORDER BY event_timestamp ASC LIMIT ";

inline constexpr std::string_view FIND_LAST_SYNC =
    "SELECT * FROM audit_log WHERE table_name IN (%1%) "
    "ORDER BY event_timestamp DESC LIMIT 1";
} // namespace audit_log_query

struct AuditLogCreateInput
{
  std::optional<int64_t> createUserId;
  int64_t recordId{0};
  TableName tableName{TableName::User};
  Json::Value changes;
  AuditLogPriority priority{AuditLogPriority::Medium};
  int64_t eventTimestamp{0};
};

struct AuditLogSyncFilter
{
  std::vector<TableName> tableNames;
  std::optional<int64_t> startTime;
  std::optional<int64_t> endTime;
};

struct AuditLogWriteInput
{
  int64_t recordId{0};
  TableName tableName{TableName::User};
  ChangesDiff changes;
  AuditLogPriority priority{AuditLogPriority::Medium};
  std::optional<int64_t> createUserId;
};

struct AuditLogFindExistInput
{
  int64_t recordId{0};
  TableName tableName{TableName::User};
  int64_t dayStart{0};
  int64_t dayEnd{0};
};

struct AuditLogUpdateInput
{
  int64_t id{0};
  Json::Value changes;
  int64_t eventTimestamp{0};
};
