#pragma once

#include <drogon/HttpRequest.h>
#include <feature/auth/services/session-events.hxx>
#include <string>
#include <validation/validation_dsl.hxx>

struct RevokeSessionsDto
{
  std::string scope;
  SessionRevocationScope target{SessionRevocationScope::Others};

  static RevokeSessionsDto fromRequest(const drogon::HttpRequestPtr& request);
};
