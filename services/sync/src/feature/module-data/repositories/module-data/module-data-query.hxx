#pragma once

#include <array>
#include <drogon/orm/DbClient.h>
#include <string>
#include <string_view>

namespace module_data_query
{

inline constexpr std::string_view SUMMARY =
    "SELECT "
    "(SELECT COUNT(*) FROM audit_log WHERE table_name IN (SELECT value FROM json_each(?1))) + "
    "(SELECT COUNT(*) FROM user_audit_log WHERE table_name IN (SELECT value FROM json_each(?1))) + "
    "(SELECT COUNT(*) FROM user_action_log WHERE table_name IN (SELECT value FROM json_each(?1))) AS history, "
    "(SELECT COALESCE(SUM(?2 + length(changes)), 0) FROM audit_log "
    "WHERE table_name IN (SELECT value FROM json_each(?1))) + "
    "(SELECT COALESCE(SUM(?2 + length(changes)), 0) FROM user_audit_log "
    "WHERE table_name IN (SELECT value FROM json_each(?1))) + "
    "(SELECT COALESCE(SUM(?2 + length(old_data) + length(new_data)), 0) FROM user_action_log "
    "WHERE table_name IN (SELECT value FROM json_each(?1))) AS bytes";

inline constexpr std::array<std::string_view, 4> PURGE{
    "DELETE FROM audit_log WHERE table_name IN (SELECT value FROM json_each(?))",
    "DELETE FROM user_audit_log WHERE table_name IN (SELECT value FROM json_each(?))",
    "DELETE FROM user_action_log WHERE table_name IN (SELECT value FROM json_each(?))",
    "DELETE FROM audit_compaction_state WHERE table_name IN (SELECT value FROM json_each(?))"};

inline constexpr std::int64_t kRowOverheadBytes = 48;

}

struct ModuleHistoryInput
{
  std::string tables;
  drogon::orm::DbClient* client{nullptr};
};
