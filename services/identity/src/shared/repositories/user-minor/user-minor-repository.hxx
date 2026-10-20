#pragma once

#include "user-minor-query.hxx"

#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/user-minor/user-minor-schema.hxx>
#include <vector>

class UserMinorRepository
{
public:
  UserMinorRepository() = default;

  drogon::Task<std::optional<UserMinorSchema>> find(int64_t userId, drogon::orm::DbClient* client = nullptr) const;

  drogon::Task<std::vector<UserMinorSchema>> findAll(drogon::orm::DbClient* client = nullptr) const;

  drogon::Task<std::optional<UserMinorSchema>> upsert(const UserMinorUpsertInput& input) const;

  drogon::Task<bool> remove(int64_t userId, drogon::orm::DbClient* client = nullptr) const;
};
