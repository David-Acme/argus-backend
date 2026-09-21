#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>

// The browser-facing headers every service answers with, and the pre-routing
// branch that answers an OPTIONS probe. Both belong to the substrate: no
// domain decides them and no handler throws them.
class Cors
{
public:
  static void apply(const drogon::HttpResponsePtr& resp);

  // Registered as a pre-routing advice by every service; it answers and ends
  // the request, so the response never reaches a handler.
  static void
  handleOptions(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};
