#include "jwt-filter.hxx"

#include <shared/exceptions/response-exception.hxx>

drogon::Task<drogon::HttpResponsePtr>
JwtFilter::doFilter(const drogon::HttpRequestPtr& req)
{
  // TODO: extract and validate JWT, populate JwtContext, inject attribute.
  co_return drogon::HttpResponsePtr{};
}
