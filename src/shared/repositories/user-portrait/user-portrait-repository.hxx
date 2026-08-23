#pragma once

#include "user-portrait-query.hxx"

#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/user-portrait/user-portrait-schema.hxx>

class UserPortraitRepository
{
public:
  UserPortraitRepository() = default;

  drogon::Task<std::optional<UserPortraitSchema>>
  findByUserId(int64_t userId) const;
  drogon::Task<UserPortraitSchema>
  upsertCurrent(const UserPortraitUpsertInput& input) const;
};
