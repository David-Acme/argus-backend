#pragma once

#include "refresh-token-query.hxx"

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/refresh-token/refresh-token-schema.hxx>
#include <string>

class RefreshTokenRepository
{
public:
  RefreshTokenRepository() = default;
  ~RefreshTokenRepository() = default;

  drogon::Task<RefreshTokenSchema>
  create(const RefreshTokenCreateInput& input) const;

  drogon::Task<std::optional<RefreshTokenSchema>> findById(int64_t id) const;

  drogon::Task<std::optional<RefreshTokenSchema>>
  findByAccessToken(int64_t userId, const std::string& accessToken) const;

  drogon::Task<std::optional<RefreshTokenSchema>>
  findByRefreshToken(int64_t userId, const std::string& refreshToken) const;

  drogon::Task<bool> invalidate(int64_t id) const;
  drogon::Task<bool> markUsed(int64_t id) const;
  drogon::Task<bool> invalidateAllUser(int64_t userId) const;
};
