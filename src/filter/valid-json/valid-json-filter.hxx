#pragma once

#include <drogon/HttpFilter.h>
#include <drogon/utils/coroutine.h>

class ValidJsonFilter : public drogon::HttpCoroFilter<ValidJsonFilter>
{
public:
  drogon::Task<drogon::HttpResponsePtr>
  doFilter(const drogon::HttpRequestPtr& req) override;
};
