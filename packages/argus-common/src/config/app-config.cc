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
  return ApiResponse::error({.statusCode = 400,
                              .errorCode = ERROR_CODE_BAD_REQUEST,
                              .message = message});
}

drogon::HttpResponsePtr AppConfig::get401Response(const std::string& message)
{
  return ApiResponse::error({.statusCode = 401,
                              .errorCode = ERROR_CODE_UNAUTHORIZED,
                              .message = message});
}

drogon::HttpResponsePtr AppConfig::get403Response(const std::string& message)
{
  return ApiResponse::error({.statusCode = 403,
                              .errorCode = ERROR_CODE_FORBIDDEN,
                              .message = message});
}

drogon::HttpResponsePtr AppConfig::get404Response(const std::string& message)
{
  return ApiResponse::error({.statusCode = 404,
                              .errorCode = ERROR_CODE_NOT_FOUND,
                              .message = message});
}

drogon::HttpResponsePtr AppConfig::get405Response(const std::string& message)
{
  return ApiResponse::error({.statusCode = 405,
                              .errorCode = ERROR_CODE_METHOD_NOT_ALLOWED,
                              .message = message});
}

drogon::HttpResponsePtr AppConfig::get409Response(const std::string& message)
{
  return ApiResponse::error({.statusCode = 409,
                              .errorCode = ERROR_CODE_CONFLICT,
                              .message = message});
}

drogon::HttpResponsePtr AppConfig::get429Response(const std::string& message)
{
  return ApiResponse::error({.statusCode = 429,
                              .errorCode = ERROR_CODE_TOO_MANY_REQUESTS,
                              .message = message});
}

drogon::HttpResponsePtr
AppConfig::get500Response(const std::string& message,
                          const std::string& errorCode)
{
  return ApiResponse::error({.statusCode = 500,
                              .errorCode = errorCode,
                              .message = message});
}

drogon::HttpResponsePtr
AppConfig::get502Response(const std::string& message,
                          const std::string& errorCode)
{
  return ApiResponse::error({.statusCode = 502,
                              .errorCode = errorCode,
                              .message = message});
}

drogon::HttpResponsePtr
AppConfig::get503Response(const std::string& message,
                          const std::string& errorCode)
{
  return ApiResponse::error({.statusCode = 503,
                              .errorCode = errorCode,
                              .message = message});
}

drogon::HttpResponsePtr AppConfig::getRemoteNotAllowedResponse()
{
  return ApiResponse::error({.statusCode = 403,
                              .errorCode = ERROR_CODE_REMOTE_NOT_ALLOWED,
                              .message = "Remote requests are not allowed"});
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
        ApiResponse::error({.statusCode = re->statusCode(),
                            .errorCode = re->errorCode(),
                            .message = re->what()}));
    return;
  }

  respCallback(ApiResponse::error({.statusCode = 500,
                                   .errorCode = "INTERNAL_ERROR",
                                   .message = e.what()}));
}
