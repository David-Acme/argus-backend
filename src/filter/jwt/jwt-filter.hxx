#pragma once

#include <drogon/HttpFilter.h>
#include <drogon/utils/coroutine.h>
#include <string>

struct JwtContext
{
  std::string sub;
  std::string email;
};

class JwtFilter : public drogon::HttpCoroFilter<JwtFilter>
{
public:
  drogon::Task<drogon::HttpResponsePtr>
  doFilter(const drogon::HttpRequestPtr& req) override;
};
