#include "valid-json-filter.hxx"

#include <auth/auth-errors.hxx>
#include <errors/response-exception.hxx>

drogon::Task<drogon::HttpResponsePtr>
ValidJsonFilter::doFilter(const drogon::HttpRequestPtr& req)
{
  auto method = req->method();
  if (method == drogon::Post || method == drogon::Patch) {
    if (!req->getJsonError().empty() || req->getJsonObject() == nullptr) {
      throw ResponseException(AuthErrors::InvalidJsonBody);
    }
  }
  co_return drogon::HttpResponsePtr{};
}
