#pragma once

#include <json/value.h>
#include <shared/enums.hxx>
#include <string>
#include <string_view>

namespace user_action_log_query
{
inline constexpr std::string_view INSERT =
    "INSERT INTO user_action_log (user_id, record_id, table_name, action, "
    "old_data, new_data, ip_address) VALUES (?, ?, ?, ?, ?, ?, ?)";
} // namespace user_action_log_query

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
