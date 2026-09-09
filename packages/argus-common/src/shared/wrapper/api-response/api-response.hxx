#pragma once

#include <drogon/HttpResponse.h>
#include <json/value.h>
#include <string>

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

class ApiResponse
{
public:
  static drogon::HttpResponsePtr ok(const Json::Value& data = Json::Value());
  static drogon::HttpResponsePtr created(const Json::Value& data);
  static drogon::HttpResponsePtr noContent();

  static drogon::HttpResponsePtr error(const ErrorInput& input);

  static drogon::HttpResponsePtr
  validationError(const Json::Value& fieldErrors);

private:
  static drogon::HttpResponsePtr json(const JsonInput& input);
};
