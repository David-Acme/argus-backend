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
  static drogon::HttpResponsePtr
  get405Response(const std::string& message = "Method not allowed");
  static drogon::HttpResponsePtr
  get409Response(const std::string& message = "Conflict");

  static void handleException(
      const std::exception& e, const drogon::HttpRequestPtr& req,
      std::function<void(const drogon::HttpResponsePtr&)>&& respCallback);

  static inline const std::string JWT_CTX_KEY{"jwt_ctx"};
  static inline const std::string DEVICE_CTX_KEY{"device_ctx"};

  static inline const std::string SYNC_LIMIT{"200"};

  static inline const std::string ERROR_CODE_BAD_REQUEST{"BAD_REQUEST"};
  static inline const std::string ERROR_CODE_UNAUTHORIZED{"UNAUTHORIZED"};
  static inline const std::string ERROR_CODE_FORBIDDEN{"FORBIDDEN"};
  static inline const std::string ERROR_CODE_NOT_FOUND{"NOT_FOUND"};
  static inline const std::string ERROR_CODE_METHOD_NOT_ALLOWED{
      "METHOD_NOT_ALLOWED"};
  static inline const std::string ERROR_CODE_CONFLICT{"CONFLICT"};
};
