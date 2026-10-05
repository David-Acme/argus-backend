#pragma once

#include <cstdint>
#include <drogon/HttpRequest.h>
#include <string>

struct ListVisitorsDto
{
  std::string scope;
  std::string filter;
  std::string search;
  int64_t limit{500};
  int64_t beforeSeen{0};
  int64_t beforeId{0};

  static ListVisitorsDto fromRequest(const drogon::HttpRequestPtr& request);
};
