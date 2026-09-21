#include "api-response.hxx"

#include <errors/error-definition.hxx>
#include <errors/response-exception.hxx>
#include <http/details/http-errors.hxx>

#include <drogon/HttpResponse.h>
#include <json/value.h>

#include <string>
#include <utility>

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
  Json::Value err;
  err["code"] = input.errorCode;
  err["message"] = input.message;
  return json({.status = input.statusCode, .info = nullptr, .errors = &err});
}

drogon::HttpResponsePtr ApiResponse::error(const ErrorDefinition& error)
{
  Json::Value err;
  err["code"] = std::string(error.wireCode());
  err["message"] = std::string(error.message);
  return json({.status = error.status, .info = nullptr, .errors = &err});
}

drogon::HttpResponsePtr ApiResponse::error(const ResponseException& error)
{
  const auto serialize = [](const ResponseError& value) {
    Json::Value result(Json::objectValue);
    result["code"] = value.code;
    result["message"] = value.message;
    return result;
  };

  Json::Value errors;
  if (const auto* single = std::get_if<ResponseError>(&error.errors())) {
    errors = serialize(*single);
  }
  else {
    errors = Json::Value(Json::arrayValue);
    for (const auto& item : std::get<std::vector<ResponseError>>(error.errors()))
      errors.append(serialize(item));
  }
  return json({.status = error.statusCode(), .info = nullptr, .errors = &errors});
}

drogon::HttpResponsePtr
ApiResponse::validationError(const Json::Value& fieldErrors)
{
  Json::Value err;
  err["code"] = std::string(HttpErrors::ValidationFailed.wireCode());
  err["message"] = std::string(HttpErrors::ValidationFailed.message);
  err["fields"] = fieldErrors;
  return json(
      {.status = HttpErrors::ValidationFailed.status, .info = nullptr,
       .errors = &err});
}

drogon::HttpResponsePtr ApiResponse::json(const JsonInput& input)
{
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(static_cast<drogon::HttpStatusCode>(input.status));
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);

  Json::Value body;
  body["status"] = input.status;
  body["info"] = input.info ? *input.info : Json::Value();
  body["errors"] = input.errors ? *input.errors : Json::Value();

  resp->setBody(body.toStyledString());
  return resp;
}
