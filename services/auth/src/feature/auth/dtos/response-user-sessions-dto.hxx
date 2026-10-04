#pragma once

#include <cstdint>
#include <feature/auth/dtos/response-list-sessions-dto.hxx>
#include <json/value.h>
#include <vector>

struct UserSessionsView
{
  int64_t userId{0};
  std::vector<SessionView> sessions;
};

struct ResponseUserSessionsDto
{
  std::vector<UserSessionsView> users;

  [[nodiscard]] Json::Value toJson() const;
};
