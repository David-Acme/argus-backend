#pragma once

#include <cstdint>
#include <drogon/HttpRequest.h>

// Query DTO for GET /notification/delivery-summary; bounds the latency
// window. Absent means the trailing 24 hours.
struct DeliverySummaryDto
{
  int64_t since{0};

  static DeliverySummaryDto fromRequest(const drogon::HttpRequestPtr& request);
};
