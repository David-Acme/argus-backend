#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/session/schemas/refresh-token/refresh-token-schema.hxx>
#include <optional>
#include <string>

class RefreshTokenRepository
{
public:
  RefreshTokenRepository() = default;
  ~RefreshTokenRepository() = default;

  [[nodiscard]] drogon::Task<std::optional<RefreshTokenSchema>>
  findByAccessToken(int64_t userId, const std::string& accessToken) const;

  [[nodiscard]] drogon::Task<bool> invalidateAllUser(int64_t userId) const;
};
