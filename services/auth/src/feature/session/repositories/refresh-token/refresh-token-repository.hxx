#pragma once

#include "refresh-token-query.hxx"

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <feature/session/schemas/refresh-token/refresh-token-schema.hxx>
#include <optional>
#include <string>

class RefreshTokenRepository
{
public:
  RefreshTokenRepository() = default;
  ~RefreshTokenRepository() = default;

  [[nodiscard]] drogon::Task<RefreshTokenSchema>
  create(const RefreshTokenCreateInput& input) const;

  [[nodiscard]] drogon::Task<std::optional<RefreshTokenSchema>>
  findByAccessToken(int64_t userId, const std::string& accessToken) const;

  [[nodiscard]] drogon::Task<std::optional<RefreshTokenSchema>>
  findByRefreshToken(int64_t userId, const std::string& refreshToken) const;

  [[nodiscard]] drogon::Task<bool> markUsed(int64_t id) const;

  [[nodiscard]] drogon::Task<bool>
  invalidateAllUser(int64_t userId,
                    drogon::orm::DbClient* client = nullptr) const;

  [[nodiscard]] drogon::Task<void> pruneStale(int64_t userId) const;
};
