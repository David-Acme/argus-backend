#pragma once

#include <drogon/HttpRequest.h>
#include <optional>
#include <string>

struct RemovePinDto
{
  std::optional<std::string> currentPin;

  static RemovePinDto fromRequest(const drogon::HttpRequestPtr& req);
};
