#pragma once

#include <drogon/HttpRequest.h>
#include <string>
#include <string_view>
#include <validation/validation_dsl.hxx>

struct IdempotencyKeyDto
{
  static constexpr std::string_view kHeader = "Idempotency-Key";

  std::string key;

  static IdempotencyKeyDto fromRequest(const drogon::HttpRequestPtr& request);
};
