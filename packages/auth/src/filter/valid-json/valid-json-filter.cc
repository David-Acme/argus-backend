#include "valid-json-filter.hxx"

#include <config/app-config.hxx>

drogon::Task<drogon::HttpResponsePtr>
ValidJsonFilter::doFilter(const drogon::HttpRequestPtr& req)
{
  auto method = req->method();
  if (method == drogon::Post || method == drogon::Patch) {
    if (!req->getJsonError().empty() || req->getJsonObject() == nullptr) {
      co_return AppConfig::get400Response("Invalid JSON body");
    }
  }
  co_return drogon::HttpResponsePtr{};
}
