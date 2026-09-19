#pragma once

#include <cstdint>
#include <drogon/HttpRequest.h>

// Query DTO for DELETE /guard/expected-guests?id=...
struct RemoveExpectedGuestDto
{
  int64_t id{0};

  static RemoveExpectedGuestDto fromRequest(
      const drogon::HttpRequestPtr& request);
};
