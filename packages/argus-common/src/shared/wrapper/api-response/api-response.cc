#include "api-response.hxx"

#include <drogon/HttpTypes.h>

drogon::HttpResponsePtr ApiResponse::ok(const Json::Value& data)
{
  return json({.status = 200, .info = &data, .errors = nullptr});
}

drogon::HttpResponsePtr ApiResponse::created(const Json::Value& data)
{
  return json({.status = 201, .info = &data, .errors = nullptr});
}

drogon::HttpResponsePtr ApiResponse::noContent()
{
  return json({.status = 204, .info = nullptr, .errors = nullptr});
}

drogon::HttpResponsePtr ApiResponse::error(const ErrorInput& input)
{
  const int statusCode = input.statusCode;
  const std::string& errorCode = input.errorCode;
  const std::string& message = input.message;

  Json::Value err;
  err["code"] = errorCode;
  err["message"] = message;
  return json({.status = statusCode, .info = nullptr, .errors = &err});
}

drogon::HttpResponsePtr
ApiResponse::validationError(const Json::Value& fieldErrors)
{
  Json::Value err;
  err["code"] = "VALIDATION_ERROR";
  err["message"] = "Validation failed";
  err["fields"] = fieldErrors;
  return json({.status = 422, .info = nullptr, .errors = &err});
}

drogon::HttpResponsePtr ApiResponse::json(const JsonInput& input)
{
  const int status = input.status;
  const Json::Value* info = input.info;
  const Json::Value* errors = input.errors;
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);

  Json::Value body;
  body["status"] = status;
  body["info"] = info ? *info : Json::Value();
  body["errors"] = errors ? *errors : Json::Value();

  auto str = body.toStyledString();
  resp->setBody(std::move(str));
  return resp;
}
