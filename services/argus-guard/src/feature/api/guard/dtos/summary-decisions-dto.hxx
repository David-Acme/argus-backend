#pragma once

#include <cstdint>
#include <drogon/HttpRequest.h>

// Query DTO for GET /guard/decisions/summary; bounds an optional time window.
struct SummaryDecisionsDto
{
  int64_t from{0};
  int64_t to{0};
  int nearMissMargin{0};

  static SummaryDecisionsDto fromRequest(const drogon::HttpRequestPtr& request);
};
