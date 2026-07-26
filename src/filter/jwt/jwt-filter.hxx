#pragma once

#include <drogon/HttpFilter.h>
#include <drogon/utils/coroutine.h>
#include <shared/enums.hxx>
#include <string>

struct JwtContext
{
  int64_t userId{0};
  int64_t featureHubId{0};
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
};
