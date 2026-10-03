#pragma once

#include <drogon/HttpRequest.h>
#include <string>
#include <validation/validation_dsl.hxx>

struct StartDeviceLoginDto
{
  std::string pollHash;

  static StartDeviceLoginDto fromRequest(const drogon::HttpRequestPtr& request);
};
