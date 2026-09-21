#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>

#include <exception>
#include <functional>

// The one advice every service registers (decision D20). It is the single
// place a refusal becomes an envelope: a validation failure, a typed
// ResponseException, or anything else that reached the top untyped.
//
// The two answers the framework asks for directly -- an unmatched route's 404
// and 405 -- come from here too, because they are produced with no exception to
// carry them.
class ErrorHandler
{
public:
  static void handleException(
      const std::exception& e, const drogon::HttpRequestPtr& req,
      std::function<void(const drogon::HttpResponsePtr&)>&& respCallback);

  static drogon::HttpResponsePtr
  unmatchedRoute(drogon::HttpStatusCode status);
};
