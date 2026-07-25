#pragma once

#include <drogon/HttpResponse.h>
#include <json/value.h>
#include <string>

class ApiResponse
{
public:
  static drogon::HttpResponsePtr ok(const Json::Value& data = Json::Value());
  static drogon::HttpResponsePtr created(const Json::Value& data);
  static drogon::HttpResponsePtr noContent();

  static drogon::HttpResponsePtr error(int statusCode,
                                       const std::string& errorCode,
                                       const std::string& message);

private:
  static drogon::HttpResponsePtr json(int status, const Json::Value* info,
                                      const Json::Value* errors);
};
