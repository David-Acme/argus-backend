#pragma once

#include <drogon/HttpRequest.h>
#include <feature/session/services/session-events.hxx>
#include <string>
#include <validation/validation_dsl.hxx>

struct RevokeSessionsDto
{
  std::string scope;
  SessionRevocationScope target{SessionRevocationScope::Others};

  static RevokeSessionsDto fromRequest(const drogon::HttpRequestPtr& request);
};
