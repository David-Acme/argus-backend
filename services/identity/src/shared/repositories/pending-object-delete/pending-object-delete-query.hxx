#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>
#include <string_view>
#include <vector>

namespace pending_object_delete_query
{
inline constexpr std::string_view ENQUEUE =
    "INSERT INTO pending_object_delete (object_key, attempts, next_attempt_at) "
    "SELECT value, 0, 0 FROM json_each(?) WHERE value != '' "
    "ON CONFLICT (object_key) DO NOTHING";

inline constexpr std::string_view FIND_DUE =
    "SELECT id, object_key, attempts FROM pending_object_delete "
    "WHERE next_attempt_at <= ? ORDER BY next_attempt_at, id LIMIT ?";

inline constexpr std::string_view REMOVE =
    "DELETE FROM pending_object_delete WHERE id = ?";

inline constexpr std::string_view POSTPONE =
    "UPDATE pending_object_delete SET attempts = attempts + 1, "
    "next_attempt_at = ? WHERE id = ?";

inline constexpr std::string_view COUNT =
    "SELECT COUNT(*) AS pending FROM pending_object_delete";
}

struct PendingObjectEnqueueInput
{
  std::vector<std::string> objectKeys;
  drogon::orm::DbClient* client{nullptr};
};

struct PendingObjectDueInput
{
  int64_t now{0};
  int64_t limit{0};
};

struct PendingObjectPostponeInput
{
  int64_t id{0};
  int64_t nextAttemptAt{0};
};

struct PendingObjectDelete
{
  int64_t id{0};
  std::string objectKey;
  int64_t attempts{0};
};
