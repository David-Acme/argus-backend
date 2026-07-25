#pragma once

#include <drogon/HttpResponse.h>
#include <functional>

class AppConfig
{
public:
  static void applyCors(const drogon::HttpResponsePtr& resp);

  static void
  handleOptions(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback);

  static drogon::HttpResponsePtr get404Response();

  static void handleException(
      const std::exception& e, const drogon::HttpRequestPtr& req,
      std::function<void(const drogon::HttpResponsePtr&)>&& respCallback);
};
