#pragma once

#include <errors/error-definition.hxx>
#include <errors/response-exception.hxx>

#include <drogon/HttpResponse.h>
#include <json/value.h>

#include <string>

// An envelope built from strings that were never a catalog entry: the gateway
// relays a downstream service's own code, so there is nothing to look up.
struct ErrorInput
{
  int statusCode;
  const std::string& errorCode;
  const std::string& message;
};

struct JsonInput
{
  int status;
  const Json::Value* info;
  const Json::Value* errors;
};

// The one envelope every service answers with: {status, info, errors}. Nothing
// outside this class sets a status code or a body, so the wire shape cannot
// drift service by service (architecture plan section 4.7).
class ApiResponse
{
public:
  static drogon::HttpResponsePtr ok(const Json::Value& data = Json::Value());
  static drogon::HttpResponsePtr created(const Json::Value& data);
  static drogon::HttpResponsePtr noContent();

  static drogon::HttpResponsePtr error(const ErrorInput& input);
  static drogon::HttpResponsePtr error(const ErrorDefinition& error);
  static drogon::HttpResponsePtr error(const ResponseException& error);

  static drogon::HttpResponsePtr
  validationError(const Json::Value& fieldErrors);

private:
  static drogon::HttpResponsePtr json(const JsonInput& input);
};
