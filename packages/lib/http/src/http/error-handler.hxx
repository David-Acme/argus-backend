#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>

#include <exception>
#include <functional>

class ErrorHandler
{
public:
  static void handleException(
      const std::exception& e, const drogon::HttpRequestPtr& req,
      std::function<void(const drogon::HttpResponsePtr&)>&& respCallback);

  static drogon::HttpResponsePtr
  unmatchedRoute(drogon::HttpStatusCode status);
};
