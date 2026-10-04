#pragma once

#include <drogon/HttpFilter.h>
#include <drogon/utils/coroutine.h>
#include <auth/jwt-service.hxx>
#include <string>
#include <auth/user-role.hxx>

struct JwtContext
{
  int64_t sub{0};
  std::string name;
  UserRole role{UserRole::Guest};
  bool isActive{false};
  std::string deviceHash;
  std::string sessionId;
};

class JwtFilter : public drogon::HttpCoroFilter<JwtFilter, false>
{
public:
  drogon::Task<drogon::HttpResponsePtr>
  doFilter(const drogon::HttpRequestPtr& req) override;

  static std::string extractToken(const drogon::HttpRequestPtr& req);

private:
  JwtService jwtService_;
};
