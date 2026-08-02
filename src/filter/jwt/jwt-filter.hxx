#pragma once

#include <drogon/HttpFilter.h>
#include <drogon/utils/coroutine.h>
#include <shared/enums.hxx>
#include <shared/repositories/refresh-token/refresh-token-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/services/jwt/jwt-service.hxx>
#include <string>

struct JwtContext
{
  int64_t sub{0};
  std::string name;
  UserRole role{UserRole::Guest};
  bool isActive{false};
};

class JwtFilter : public drogon::HttpCoroFilter<JwtFilter>
{
public:
  drogon::Task<drogon::HttpResponsePtr>
  doFilter(const drogon::HttpRequestPtr& req) override;

private:
  std::string extractToken(const drogon::HttpRequestPtr& req) const;

  JwtService jwtService_;
  UserRepository userRepository_;
  RefreshTokenRepository refreshTokenRepository_;
};
