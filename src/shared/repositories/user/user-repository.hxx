#pragma once

#include "user-query.hxx"
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <shared/contracts/syncable.hxx>
#include <shared/schemas/user/user-schema.hxx>
#include <vector>

class UserRepository : public Syncable
{
public:
  UserRepository() = default;
  ~UserRepository() override = default;

  drogon::Task<std::optional<UserSchema>> findById(int64_t id) const;
  drogon::Task<UserSchema> create(const UserCreateInput& input) const;
  drogon::Task<UserSchema> update(int64_t id,
                                  const UserUpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id) const;

  drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const override;
  drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>> findLast() const override;
  drogon::Task<std::optional<Json::Value>> findLastDeleted() const override;
};
