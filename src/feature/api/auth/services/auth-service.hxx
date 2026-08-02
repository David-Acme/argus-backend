#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/api/auth/dtos/login-dto.hxx>
#include <feature/api/auth/dtos/refresh-token-dto.hxx>
#include <feature/api/auth/dtos/response-login-dto.hxx>
#include <feature/api/auth/dtos/response-refresh-token-dto.hxx>
#include <shared/repositories/person/person-repository.hxx>
#include <shared/repositories/refresh-token/refresh-token-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/services/jwt/jwt-service.hxx>
#include <shared/services/user-action-log/user-action-log-service.hxx>
#include <string>

struct LoginDeviceInput
{
  std::string deviceHash;
  std::string userAgent;
};

class AuthService
{
public:
  AuthService() = default;
  ~AuthService() = default;

  drogon::Task<ResponseLoginDto>
  login(LoginDto body, const LoginDeviceInput& device) const;

  drogon::Task<ResponseRefreshTokenDto>
  refreshToken(const RefreshTokenDto& body,
               const std::string& deviceHash) const;

  drogon::Task<void> logout(int64_t userId) const;

private:
  JwtService jwtService_;
  PersonRepository personRepository_;
  UserRepository userRepository_;
  RefreshTokenRepository refreshTokenRepository_;
  UserActionLogService userActionLogService_;
};
