#pragma once

#include <cstdint>
#include <drogon/HttpRequest.h>

struct ClipPageDto
{
  int64_t beforeCreatedAt{0};
  int64_t beforeId{0};
  int64_t limit{50};

  static ClipPageDto fromRequest(const drogon::HttpRequestPtr& request);
};
