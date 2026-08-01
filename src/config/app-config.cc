#include "app-config.hxx"

#include <shared/exceptions/response-exception.hxx>
#include <shared/validation/validator.hxx>
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
  return ApiResponse::error(400, ERROR_CODE_BAD_REQUEST, message);
}

drogon::HttpResponsePtr AppConfig::get401Response(const std::string& message)
{
  return ApiResponse::error(401, ERROR_CODE_UNAUTHORIZED, message);
}

drogon::HttpResponsePtr AppConfig::get403Response(const std::string& message)
{
  return ApiResponse::error(403, ERROR_CODE_FORBIDDEN, message);
}

drogon::HttpResponsePtr AppConfig::get404Response(const std::string& message)
{
  return ApiResponse::error(404, ERROR_CODE_NOT_FOUND, message);
}

drogon::HttpResponsePtr AppConfig::get405Response(const std::string& message)
{
  return ApiResponse::error(405, ERROR_CODE_METHOD_NOT_ALLOWED, message);
}

void AppConfig::handleException(
    const std::exception& e, const drogon::HttpRequestPtr&,
    std::function<void(const drogon::HttpResponsePtr&)>&& respCallback)
{
  if (const auto* ve = dynamic_cast<const ValidationException*>(&e)) {
    Json::Value errors;
    for (const auto& [field, msgs] : ve->errors()) {
      Json::Value arr(Json::arrayValue);
      for (const auto& m : msgs)
        arr.append(m);
      errors[field] = arr;
    }
    respCallback(ApiResponse::validationError(errors));
    return;
  }

  if (const auto* re = dynamic_cast<const ResponseException*>(&e)) {
    respCallback(
        ApiResponse::error(re->statusCode(), re->errorCode(), re->what()));
    return;
  }

  respCallback(ApiResponse::error(500, "INTERNAL_ERROR", e.what()));
}
