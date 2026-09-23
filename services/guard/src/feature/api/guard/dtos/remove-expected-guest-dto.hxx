#pragma once

#include <cstdint>
#include <drogon/HttpRequest.h>

struct RemoveExpectedGuestDto
{
  int64_t id{0};

  static RemoveExpectedGuestDto fromRequest(
      const drogon::HttpRequestPtr& request);
};
