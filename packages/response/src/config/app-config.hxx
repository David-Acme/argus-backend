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
  static drogon::HttpResponsePtr
  get429Response(const std::string& message = "Too many requests");
  static drogon::HttpResponsePtr
  get500Response(const std::string& message = "Internal error",
                 const std::string& errorCode = ERROR_CODE_INTERNAL_ERROR);
  static drogon::HttpResponsePtr
  get502Response(const std::string& message = "Upstream unreachable",
                 const std::string& errorCode = ERROR_CODE_BAD_GATEWAY);
  static drogon::HttpResponsePtr
  get503Response(const std::string& message = "Service unavailable",
                 const std::string& errorCode =
                     ERROR_CODE_SERVICE_UNAVAILABLE);

  static drogon::HttpResponsePtr getRemoteNotAllowedResponse();

  static void handleException(
      const std::exception& e, const drogon::HttpRequestPtr& req,
      std::function<void(const drogon::HttpResponsePtr&)>&& respCallback);

  static inline const std::string JWT_CTX_KEY{"jwt_ctx"};
  static inline const std::string DEVICE_CTX_KEY{"device_ctx"};
  static inline const std::string REMOTE_CTX_KEY{"remote_ctx"};

  static inline const std::string SYNC_LIMIT{"200"};

  static inline const std::string ERROR_CODE_BAD_REQUEST{"BAD_REQUEST"};
  static inline const std::string ERROR_CODE_UNAUTHORIZED{"UNAUTHORIZED"};
  static inline const std::string ERROR_CODE_FORBIDDEN{"FORBIDDEN"};
  static inline const std::string ERROR_CODE_NOT_FOUND{"NOT_FOUND"};
  static inline const std::string ERROR_CODE_METHOD_NOT_ALLOWED{
      "METHOD_NOT_ALLOWED"};
  static inline const std::string ERROR_CODE_CONFLICT{"CONFLICT"};
  static inline const std::string ERROR_CODE_REMOTE_NOT_ALLOWED{
      "REMOTE_NOT_ALLOWED"};
  static inline const std::string ERROR_CODE_SERVICE_UNAVAILABLE{
      "SERVICE_UNAVAILABLE"};
  static inline const std::string ERROR_CODE_TOO_MANY_REQUESTS{
      "TOO_MANY_REQUESTS"};
  static inline const std::string ERROR_CODE_INTERNAL_ERROR{
      "INTERNAL_ERROR"};
  static inline const std::string ERROR_CODE_BAD_GATEWAY{"BAD_GATEWAY"};
};
