#pragma once

#include <cstdint>
#include <drogon/HttpRequest.h>
#include <string>

// Query DTO for GET /guard/decisions: filters, stable cursor pagination.
// All filters are optional; absent means unfiltered. The cursor is the
// (created_at, event_id) of the last row of the previous page.
struct ListDecisionsDto
{
  int limit{20};
  int64_t from{0};
  int64_t to{0};
  int64_t cameraId{0};
  std::string severity;
  std::string decisionMode;
  std::string suppressionReason;
  bool divergentOnly{false};
  int nearMissMargin{0};
  int64_t afterCreatedAt{0};
  std::string afterEventId;

  static ListDecisionsDto fromRequest(const drogon::HttpRequestPtr& request);
};
