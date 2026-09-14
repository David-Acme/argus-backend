#pragma once

#include <cstdint>
#include <drogon/HttpRequest.h>

// Query DTO for GET /guard/incidents; the limit is clamped and validated.
struct ListIncidentsDto
{
  int limit{20};

  static ListIncidentsDto fromRequest(const drogon::HttpRequestPtr& request);
};
