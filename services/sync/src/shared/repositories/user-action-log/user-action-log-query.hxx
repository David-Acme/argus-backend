#pragma once

#include <json/value.h>
#include <sync/table-name.hxx>
#include <sync/user-action.hxx>
#include <string>
#include <string_view>

namespace user_action_log_query
{
inline constexpr std::string_view INSERT =
    "INSERT OR IGNORE INTO user_action_log (user_id, record_id, table_name, "
    "action, old_data, new_data, ip_address, msg_id) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view SYNC_FIND =
    "SELECT * FROM user_action_log "
    "WHERE created_at >= ? AND created_at <= ? "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view SYNC_FIND_FROM =
    "SELECT * FROM user_action_log WHERE created_at >= ? "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view SYNC_FIND_AFTER =
    "SELECT * FROM user_action_log WHERE "
    "(created_at > ? OR (created_at = ? AND id > ?)) AND created_at <= ? "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view SYNC_FIND_AFTER_FROM =
    "SELECT * FROM user_action_log WHERE "
    "(created_at > ? OR (created_at = ? AND id > ?)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view SYNC_FIND_ALL =
    "SELECT * FROM user_action_log ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view SYNC_FIND_LAST =
    "SELECT * FROM user_action_log ORDER BY created_at DESC, id DESC LIMIT 1";

inline constexpr std::string_view COUNT_TABLE =
    "SELECT COUNT(*) AS total FROM sqlite_master "
    "WHERE type = 'table' AND name = 'user_action_log'";
inline constexpr std::string_view COUNT_MSG_ID_COLUMN =
    "SELECT COUNT(*) AS total FROM pragma_table_info('user_action_log') "
    "WHERE name = 'msg_id'";
inline constexpr std::string_view ADD_MSG_ID_COLUMN =
    "ALTER TABLE user_action_log ADD COLUMN msg_id TEXT NOT NULL DEFAULT ''";
}

struct UserActionLogCreateInput
{
  int64_t userId{0};
  int64_t recordId{0};
  TableName tableName{TableName::User};
  UserAction action{UserAction::Create};
  Json::Value oldData;
  Json::Value newData;
  std::string ipAddress;
  std::string msgId;
};
