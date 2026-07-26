#include "app-config.hxx"

#include <shared/wrapper/api-response/api-response.hxx>

void AppConfig::applyCors(const drogon::HttpResponsePtr& resp)
{
  resp->addHeader("Access-Control-Allow-Origin", "*");
  resp->addHeader("Access-Control-Allow-Methods",
                  "GET, POST, PATCH, PUT, DELETE, OPTIONS");
  resp->addHeader("Access-Control-Allow-Headers",
                  "Content-Type, Authorization, Accept");
  resp->addHeader("Access-Control-Max-Age", "86400");
}

void AppConfig::handleOptions(
    const drogon::HttpRequestPtr&,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(drogon::k200OK);
  applyCors(resp);
  callback(resp);
}

drogon::HttpResponsePtr AppConfig::get400Response(const std::string& message)
{
  return ApiResponse::error(400, "BAD_REQUEST", message);
}

drogon::HttpResponsePtr AppConfig::get401Response(const std::string& message)
{
  return ApiResponse::error(401, "UNAUTHORIZED", message);
}

drogon::HttpResponsePtr AppConfig::get403Response(const std::string& message)
{
  return ApiResponse::error(403, "FORBIDDEN", message);
}

drogon::HttpResponsePtr AppConfig::get404Response(const std::string& message)
{
  return ApiResponse::error(404, "NOT_FOUND", message);
}

void AppConfig::handleException(
    const std::exception& e, const drogon::HttpRequestPtr&,
    std::function<void(const drogon::HttpResponsePtr&)>&& respCallback)
{
  respCallback(ApiResponse::error(500, "INTERNAL_ERROR", e.what()));
}
