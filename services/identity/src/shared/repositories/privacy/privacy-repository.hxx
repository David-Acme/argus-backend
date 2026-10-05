#pragma once

#include "privacy-query.hxx"

#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/privacy/privacy-schema.hxx>
#include <vector>

class PrivacyRepository
{
public:
  PrivacyRepository() = default;

  drogon::Task<std::optional<UserPrivacySchema>>
  findUser(int64_t userId, drogon::orm::DbClient* client = nullptr) const;

  drogon::Task<std::vector<UserPrivacySchema>>
  findAllUsers(drogon::orm::DbClient* client = nullptr) const;

  drogon::Task<UserPrivacySchema>
  upsertUser(const UserPrivacyUpsertInput& input) const;

  drogon::Task<HouseholdPrivacySchema>
  household(drogon::orm::DbClient* client = nullptr) const;

  drogon::Task<HouseholdPrivacySchema>
  updateHousehold(const HouseholdPrivacyUpdateInput& input) const;
};
