#pragma once

#include <cstdint>
#include <drogon/HttpRequest.h>

struct ListEpisodesDto
{
  int limit{30};
  int64_t before{0};

  static ListEpisodesDto fromRequest(const drogon::HttpRequestPtr& request);
};
