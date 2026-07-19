#pragma once

#include <drogon/HttpResponse.h>
#include <json/value.h>

class ApiResponse
{
public:
  static drogon::HttpResponsePtr ok(const Json::Value& data = Json::nullValue);
  static drogon::HttpResponsePtr created(const Json::Value& data);
  static drogon::HttpResponsePtr noContent();
  static drogon::HttpResponsePtr error(drogon::HttpStatusCode status,
                                       const std::string& message);
};
