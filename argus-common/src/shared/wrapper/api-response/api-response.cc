#include "api-response.hxx"

#include <drogon/HttpTypes.h>

drogon::HttpResponsePtr ApiResponse::ok(const Json::Value& data)
{
  return json(200, &data, nullptr);
}

drogon::HttpResponsePtr ApiResponse::created(const Json::Value& data)
{
  return json(201, &data, nullptr);
}

drogon::HttpResponsePtr ApiResponse::noContent()
{
  return json(204, nullptr, nullptr);
}

drogon::HttpResponsePtr ApiResponse::error(int statusCode,
                                           const std::string& errorCode,
                                           const std::string& message)
{
  Json::Value err;
  err["code"] = errorCode;
  err["message"] = message;
  return json(statusCode, nullptr, &err);
}

drogon::HttpResponsePtr
ApiResponse::validationError(const Json::Value& fieldErrors)
{
  Json::Value err;
  err["code"] = "VALIDATION_ERROR";
  err["message"] = "Validation failed";
  err["fields"] = fieldErrors;
  return json(422, nullptr, &err);
}

drogon::HttpResponsePtr ApiResponse::json(int status, const Json::Value* info,
                                          const Json::Value* errors)
{
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
