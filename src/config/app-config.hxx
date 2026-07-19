#pragma once

#include <drogon/HttpResponse.h>
#include <functional>
#include <json/value.h>

class AppConfig
{
public:
  static drogon::HttpResponsePtr get404Response();
  static void handleException(const std::exception& e,
                              const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& respCallback);
};
