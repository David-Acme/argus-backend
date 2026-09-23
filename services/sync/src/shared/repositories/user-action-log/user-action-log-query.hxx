#pragma once

#include <json/value.h>
#include <sync/table-name.hxx>
#include <sync/user-action.hxx>
#include <string>
#include <string_view>

namespace user_action_log_query
{
inline constexpr std::string_view INSERT =
    "INSERT INTO user_action_log (user_id, record_id, table_name, action, "
    "old_data, new_data, ip_address) VALUES (?, ?, ?, ?, ?, ?, ?)";

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
    "SELECT * FROM user_action_log ORDER BY created_at DESC LIMIT 1";
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
};
