#pragma once

#include <drogon/HttpFilter.h>
#include <drogon/utils/coroutine.h>
#include <shared/enums.hxx>
#include <shared/services/jwt/jwt-service.hxx>
#include <string>

struct JwtContext
{
  int64_t sub{0};
  std::string name;
  UserRole role{UserRole::Guest};
  bool isActive{false};
  std::string deviceHash;
};

class JwtFilter : public drogon::HttpCoroFilter<JwtFilter, false>
{
public:
  drogon::Task<drogon::HttpResponsePtr>
  doFilter(const drogon::HttpRequestPtr& req) override;

  // Token extraction order shared with the /sync relay.
  static std::string extractToken(const drogon::HttpRequestPtr& req);

private:
  JwtService jwtService_;
};
