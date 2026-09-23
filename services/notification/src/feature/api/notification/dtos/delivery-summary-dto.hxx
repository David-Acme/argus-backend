#pragma once

#include <cstdint>
#include <drogon/HttpRequest.h>

struct DeliverySummaryDto
{
  int64_t since{0};

  static DeliverySummaryDto fromRequest(const drogon::HttpRequestPtr& request);
};
