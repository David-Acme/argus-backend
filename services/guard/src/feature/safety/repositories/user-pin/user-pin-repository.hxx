#pragma once

#include "user-pin-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>

class UserPinRepository
{
public:
  [[nodiscard]] drogon::Task<std::optional<UserPinRow>> find(int64_t userId) const;

  drogon::Task<void> upsert(const UserPinUpsertInput& input) const;

  drogon::Task<void> remove(int64_t userId,
                            drogon::orm::DbClient* client = nullptr) const;

  drogon::Task<void>
  removeAll(drogon::orm::DbClient* client = nullptr) const;
};
