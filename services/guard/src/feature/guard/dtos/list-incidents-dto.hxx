#pragma once

#include <cstdint>
#include <drogon/HttpRequest.h>

struct ListIncidentsDto
{
  int limit{20};

  static ListIncidentsDto fromRequest(const drogon::HttpRequestPtr& request);
};
