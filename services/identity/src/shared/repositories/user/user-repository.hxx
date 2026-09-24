#pragma once

#include "user-query.hxx"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <sync/syncable.hxx>
#include <shared/schemas/user/user-schema.hxx>
#include <vector>

class UserRepository : public Syncable
{
public:
  UserRepository() = default;
  ~UserRepository() override = default;

  drogon::Task<std::optional<UserSchema>>
  findById(int64_t id, drogon::orm::DbClient* client = nullptr) const;
  drogon::Task<UserSchema> create(const UserCreateInput& input) const;
  drogon::Task<UserSchema> update(int64_t id,
                                  const UserUpdateInput& input) const;
  drogon::Task<bool> remove(int64_t id) const;
  drogon::Task<bool> hasOwner() const;
  drogon::Task<bool> hasAnyUser() const;
  drogon::Task<bool> hasOtherActiveOwner(
      int64_t excludedUserId, drogon::orm::DbClient* client = nullptr) const;
  drogon::Task<std::vector<UserSchema>>
  findAll(drogon::orm::DbClient* client = nullptr) const;
  drogon::Task<std::vector<int64_t>> findNotifiableIds() const;

  drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const override;
  drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLast(const SyncFilter& filter) const override;
  drogon::Task<std::optional<Json::Value>>
  findLastDeleted(const SyncFilter& filter) const override;
};
