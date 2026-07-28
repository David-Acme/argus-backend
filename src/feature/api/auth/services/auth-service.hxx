#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/api/auth/dtos/login-dto.hxx>
#include <feature/api/auth/dtos/refresh-token-dto.hxx>
#include <feature/api/auth/dtos/response-login-dto.hxx>
#include <feature/api/auth/dtos/response-refresh-token-dto.hxx>
#include <optional>
#include <shared/repositories/person/person-repository.hxx>
#include <shared/repositories/refresh-token/refresh-token-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/services/jwt/jwt-service.hxx>
#include <string>

class AuthService
{
public:
  AuthService() = default;
  ~AuthService() = default;

  drogon::Task<std::optional<ResponseLoginDto>>
  login(const LoginDto& body, const std::string& deviceHash,
        const std::string& userAgent) const;

  drogon::Task<std::optional<ResponseRefreshTokenDto>>
  refreshToken(const RefreshTokenDto& body,
               const std::string& deviceHash) const;

  drogon::Task<void> logout(int64_t userId) const;

private:
  JwtService jwtService_;
  PersonRepository personRepository_;
  UserRepository userRepository_;
  RefreshTokenRepository refreshTokenRepository_;
};
