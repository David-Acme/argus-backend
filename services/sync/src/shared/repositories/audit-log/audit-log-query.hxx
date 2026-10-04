#pragma once
#include <drogon/orm/DbClient.h>
#include <json/value.h>
#include <optional>
#include <sync/audit-log-priority.hxx>
#include <sync/table-name.hxx>
#include <text/json-diff.hxx>
#include <string>
#include <string_view>
#include <vector>

namespace audit_log_query
{
inline constexpr std::string_view INSERT =
    "INSERT INTO audit_log (create_user_id, record_id, table_name, changes, "
    "priority, event_timestamp) VALUES (?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view FIND_EXIST =
    "SELECT * FROM (SELECT * FROM audit_log INDEXED BY idx_audit_log_record "
    "WHERE record_id = ? AND table_name = ? ORDER BY id DESC LIMIT 1) "
    "WHERE event_timestamp >= ? AND event_timestamp <= ?";

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

inline constexpr std::string_view FIND_SYNC_AFTER_ID =
    "SELECT * FROM audit_log WHERE +table_name IN (%1%) AND id > ? "
    "ORDER BY id ASC LIMIT ";

inline constexpr std::string_view FIND_SYNC_AFTER_ID_TO =
    "SELECT * FROM audit_log WHERE +table_name IN (%1%) AND id > ? AND id <= ? "
    "ORDER BY id ASC LIMIT ";

inline constexpr std::string_view FIND_LAST_SYNC =
    "SELECT * FROM audit_log WHERE +table_name IN (%1%) "
    "ORDER BY id DESC LIMIT 1";

inline constexpr std::string_view FIND_COMPACTION_PAIRS =
    "SELECT o.id AS older_id, n.id AS newer_id "
    "FROM audit_log o JOIN audit_log n ON n.id = ("
    "SELECT min(x.id) FROM audit_log x WHERE x.record_id = o.record_id "
    "AND x.table_name = o.table_name AND x.id > o.id) "
    "WHERE n.event_timestamp < ? AND o.event_timestamp < ? "
    "ORDER BY older_id ASC LIMIT ";

inline constexpr std::string_view FIND_COMPACTION_CHANGES =
    "SELECT id, changes FROM audit_log WHERE id IN (%1%)";

inline constexpr std::string_view FIND_COMPACTION_FRONTIER =
    "SELECT compacted_through_id FROM audit_compaction_state "
    "WHERE table_name = ?";

inline constexpr std::string_view COMPACT_ROW =
    "UPDATE audit_log SET changes = ? WHERE id = ?";

inline constexpr std::string_view REMOVE_IDS =
    "DELETE FROM audit_log WHERE id IN (%1%)";

inline constexpr std::string_view ADVANCE_COMPACTION_FRONTIER =
    "INSERT INTO audit_compaction_state (table_name, compacted_through_id) "
    "VALUES (?, ?) ON CONFLICT(table_name) DO UPDATE SET "
    "compacted_through_id = max(compacted_through_id, "
    "excluded.compacted_through_id)";
}

struct AuditLogCreateInput
{
  std::optional<int64_t> createUserId;
  int64_t recordId{0};
  TableName tableName{TableName::User};
  Json::Value changes;
  AuditLogPriority priority{AuditLogPriority::Medium};
  int64_t eventTimestamp{0};
  drogon::orm::DbClient* client{nullptr};
};

struct AuditLogSyncFilter
{
  std::vector<TableName> tableNames;
  std::optional<int64_t> afterId;
  std::optional<int64_t> endId;
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
  std::optional<int64_t> eventTimestamp;
  drogon::orm::DbClient* client{nullptr};
};

struct AuditLogFindExistInput
{
  int64_t recordId{0};
  TableName tableName{TableName::User};
  int64_t dayStart{0};
  int64_t dayEnd{0};
  drogon::orm::DbClient* client{nullptr};
};

struct AuditLogCompactionPair
{
  int64_t olderId{0};
  int64_t newerId{0};
};

struct AuditLogCompactInput
{
  int64_t id{0};
  Json::Value changes;
  drogon::orm::DbClient* client{nullptr};
};
