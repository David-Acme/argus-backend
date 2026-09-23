#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>

class Cors
{
public:
  static void apply(const drogon::HttpResponsePtr& resp);

  static void
  handleOptions(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};
