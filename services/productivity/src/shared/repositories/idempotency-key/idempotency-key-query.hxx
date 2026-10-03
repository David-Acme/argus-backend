#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>
#include <string_view>

namespace idempotency_key_query
{

inline constexpr std::string_view FIND =
    "SELECT route, record_id FROM idempotency_key "
    "WHERE user_id = ? AND idem_key = ? AND created_at > ?";

inline constexpr std::string_view INSERT =
    "INSERT INTO idempotency_key (user_id, idem_key, route, record_id, created_at) "
    "VALUES (?, ?, ?, ?, ?)";

inline constexpr std::string_view PURGE =
    "DELETE FROM idempotency_key WHERE created_at <= ?";

inline constexpr int64_t kWindowSeconds = 86'400;

}

struct IdempotencyKeyFindInput
{
  int64_t userId{0};
  std::string key;
  drogon::orm::DbClient* client{nullptr};
};

struct IdempotencyKeyRecord
{
  std::string route;
  int64_t recordId{0};
};

struct IdempotencyKeyRememberInput
{
  int64_t userId{0};
  std::string key;
  std::string route;
  int64_t recordId{0};
  drogon::orm::DbClient* client{nullptr};
};
