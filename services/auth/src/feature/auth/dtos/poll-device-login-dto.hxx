#pragma once

#include <drogon/HttpRequest.h>
#include <string>
#include <string_view>
#include <validation/validation_dsl.hxx>

struct PollDeviceLoginDto
{
  static constexpr std::string_view kProofHeader = "X-Argus-Login-Proof";

  std::string proof;

  static PollDeviceLoginDto fromRequest(const drogon::HttpRequestPtr& request);
};
