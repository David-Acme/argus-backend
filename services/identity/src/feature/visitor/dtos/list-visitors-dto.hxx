#pragma once

#include <drogon/HttpRequest.h>
#include <optional>
#include <string>

struct ListVisitorsDto
{
  std::string scope;

  static ListVisitorsDto fromRequest(const drogon::HttpRequestPtr& request);
};
