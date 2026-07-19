#include "valid-json-filter.hxx"

#include <shared/exceptions/response-exception.hxx>

drogon::Task<drogon::HttpResponsePtr>
ValidJsonFilter::doFilter(const drogon::HttpRequestPtr& req)
{
  auto method = req->method();
  if (method == drogon::Post || method == drogon::Patch) {
    if (!req->getJsonError().empty() || req->getJsonObject() == nullptr) {
      throw ResponseException("Invalid JSON body", 400);
    }
  }
  co_return drogon::HttpResponsePtr{};
}
