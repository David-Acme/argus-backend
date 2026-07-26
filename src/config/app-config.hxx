#pragma once

#include <drogon/HttpResponse.h>
#include <functional>
#include <string>

class AppConfig
{
public:
  static void applyCors(const drogon::HttpResponsePtr& resp);

  static void
  handleOptions(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback);

  static drogon::HttpResponsePtr
  get400Response(const std::string& message = "Bad request");
  static drogon::HttpResponsePtr
  get401Response(const std::string& message = "Authentication required");
  static drogon::HttpResponsePtr
  get403Response(const std::string& message = "Access denied");
  static drogon::HttpResponsePtr
  get404Response(const std::string& message = "Path not found");

  static void handleException(
      const std::exception& e, const drogon::HttpRequestPtr& req,
      std::function<void(const drogon::HttpResponsePtr&)>&& respCallback);

  static inline const std::string JWT_CTX_KEY{"jwt_ctx"};
  static inline const std::string DEVICE_CTX_KEY{"device_ctx"};
};
